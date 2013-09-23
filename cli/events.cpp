/*
Copyright (c) 2009-2013, Intel Corporation
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <err.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <poll.h>
#include <assert.h>`
#include <immintrin.h>
#include <map>
#include <string>
//#include <tbb/spin_rw_mutex.h>
/* We were using reader-write lock but it requires tbb runtime. */
#include <tbb/spin_mutex.h>

#include "perf_utils.h"
#include "metrics.h"

static void readcounters(int mycpu);
static void *thrstart(void *);


static pthread_t *threads;
static volatile int done;
static pthread_barrier_t barrier;

static counter *counters;
counter *lastctr;
static evinfo *events;

static double starttime;

static int perf_read_buffer(struct perf_event_mmap_page *hdr, char *buf, size_t sz);
static void perf_skip_buffer(struct perf_event_mmap_page *hdr, size_t sz);
static void kernel_module();

enum modType {modOpenMP, modMPI, modKernel, modOther, modNone};


struct modInfo {
    std::string name;
    int type;
    uint64_t base;
    uint64_t size;
};

static modInfo kernelMod;

typedef std::map<uint64_t, modInfo> modmap_t;

std::map<int, modmap_t *> modByPid;

tbb::spin_mutex modMutex;


#define NPAGES 4
#define PAGESIZE 4096


void
setup(int pid)
{
    struct perf_event_attr hw;
    struct timeval tv;
    int ret;

    kernel_module();

    memset(&hw, 0, sizeof(hw));
    hw.size = sizeof(hw);

    gbl.ncpus = sysconf(_SC_NPROCESSORS_ONLN);

    counters = (counter *)_mm_malloc(gbl.ncpus * sizeof(counter), 64);
    if (counters == NULL)
        err(1, "Can't malloc counters");
    memset(counters, 0, gbl.ncpus * sizeof(counter));
    lastctr = (counter *)_mm_malloc(gbl.ncpus * sizeof(counter), 64);
    if (lastctr == NULL)
        err(1, "Can't malloc lastctr");
    memset(lastctr, 0, gbl.ncpus * sizeof(counter));
    events = (evinfo *)_mm_malloc(gbl.ncpus * sizeof(evinfo), 64);
    if (events == 0)
        err(1, "Can't malloc events");
    threads = (pthread_t *)malloc(gbl.ncpus * sizeof(pthread_t));
    if (threads == 0)
        err(1, "Can't malloc threads");
    /* Could do this per thread */
    for (int cpu = 0; cpu < gbl.ncpus; ++cpu)
    {
        evinfo *ep = &events[cpu];
        ep->count = 0;
        hw.type = PERF_TYPE_HARDWARE;
        hw.config = PERF_COUNT_HW_CPU_CYCLES;
        hw.wakeup_events  = 5;
        hw.sample_freq = samplingFreq;
        hw.freq = 1;
        hw.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
        hw.disabled = 1;
        hw.mmap = 1;
        ep->fd = perf_event_open(&hw, -1, cpu, -1, 0);
        if (ep->fd == -1)
            err(1, "CPU %d, event 0x%lx", cpu, hw.config);
        ep->buf = mmap(NULL, (NPAGES+1)*PAGESIZE,
            PROT_READ|PROT_WRITE, MAP_SHARED, ep->fd, 0);
        if (ep->buf == MAP_FAILED)
            err(1, "CPU %d, event 0x%lx: cannot mmap buffer",
                cpu, hw.config);
    }

    pthread_barrier_init(&barrier, NULL, gbl.ncpus);
    /* Now spawn a pthread on every processor */
    for (int i = 1; i < gbl.ncpus; ++i)
    {
        pthread_attr_t attr;
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(i, &cpuset);
        pthread_attr_init(&attr);
        pthread_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset);
        ret = pthread_create(&threads[i], &attr, thrstart, (void *)i);
        if (ret)
            err(1, "pthread_create returns %d", ret);
        pthread_attr_destroy(&attr);
    }

    setup_output();
    ret = gettimeofday(&tv, 0);
    if (ret != 0)
        err(1, "gettimeofday");
    starttime = tv.tv_sec + (double)tv.tv_usec * 1e-6;
    //pthread_barrier_wait(&barrier);
}

void
jointhreads()
{
    for (int i = 1; i < gbl.ncpus; ++i)
        pthread_join(threads[i], NULL);
}

static void
kernel_module()
{
    /* get an approximation of the range of addresses occupied by the kernel
     * It would probably be fine just mark anything above 0xFF... as being in
     * the kernel,but we'll
     * try a little harder.
     */
    setuid(0);  // so the shell calls are done as root
    FILE *fp = popen("grep _stext /proc/kallsyms", "r");
    assert(fp != 0);
    char buffer[1024];
    if (fgets(buffer, sizeof(buffer), fp) == 0)
        goto bail;

    uint64_t kstart = 0;
    char *name = 0;
    if (sscanf(buffer, "%lx %*s %as\n", &kstart, &name) != 2)
        goto bail;
    assert(kstart != 0);
    if (strcmp(name, "_stext") != 0)
        goto bail;
    free(name);
    name = 0;
    fclose(fp);

    fp = popen("sort -k6 /proc/modules | tail -1", "r");
    assert(fp != 0);
    if (fgets(buffer, sizeof(buffer), fp) == 0)
        goto bail;
    uint64_t len = 0, modbase = 0;
    if (sscanf(buffer, "%*s %ld %*d %*s %*s 0x%lx\n", &len, &modbase) != 2)
        goto bail;
    assert(len != 0);
    assert(modbase != 0);
    uint64_t kend = modbase + len;
    fclose(fp);
    //printf("Kernel: start 0x%lx end 0x%lx\n", kstart, kend);
    kernelMod.name = "vmlinux";
    kernelMod.type = modKernel;
    kernelMod.base = kstart;
    kernelMod.size = kend - kstart;
    return;
bail:
    if (name) free(name);
    fclose(fp);
    return;
}

// master thread timeout >> slave threads
static void
timeout()
{
    static struct timespec t = {0, 200000000};       // 5 times per second
    nanosleep(&t, 0);
}


// This should tick faster than the master.
static void *
thrstart(void *arg)
{
    int mycpu = (int)((uint64_t)arg) & 0xFFFF;
    struct pollfd fds;

    pthread_barrier_wait(&barrier);
    fds.fd = events[mycpu].fd;
    fds.revents = 0;
    fds.events = POLLIN;
    int ret = ioctl(events[mycpu].fd, PERF_EVENT_IOC_RESET, 0);
    if (ret) 
        err(1, "ioctl reset");
    ret = ioctl(events[mycpu].fd, PERF_EVENT_IOC_ENABLE, 0);
    if (ret) 
        err(1, "ioctl enable");
    for (;;)
    {
        poll(&fds, 1, 50);
        if (fds.revents & POLLIN)
            readcounters(mycpu);

        if (done)
            break;
    }
    return 0;
}


static void
readcounters(int cpu)
{
    static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

    counter *cp = &counters[cpu];
    evinfo *ep = &events[cpu];
#if defined(KNC)
    union {
        __m512d c;
        uint64_t values[8];
    } u;
#else
    union {
        __m256d c;
        uint64_t values[4];
    } u;
#endif

    struct perf_event_header ehdr;

    while (perf_read_buffer((perf_event_mmap_page *)ep->buf, (char *)&ehdr, sizeof(ehdr)) == 0)
    {
        assert(ehdr.size >= sizeof(ehdr));
        if (ehdr.type == PERF_RECORD_SAMPLE)
        {
            struct { uint64_t ip; uint32_t pid; uint32_t tid; } sample;
            int ret;
            int type = modNone;
            modmap_t *m;

            u.values[0] = cp->counts[0];
            u.values[1] = cp->counts[1];
            u.values[2] = cp->counts[2];
            u.values[3] = cp->counts[3];
            assert(ehdr.size == sizeof(ehdr) + sizeof(sample));
            ret = perf_read_buffer((perf_event_mmap_page *)ep->buf,
                (char *)&sample, sizeof(sample));
            assert(ret == 0);
            modMutex.lock();
            std::map<int,modmap_t*>::iterator it1 = modByPid.find(sample.pid);
            m = (it1 ==  modByPid.end()) ? 0 : it1->second;
            modMutex.unlock();
            if (m == 0)
            {
                if (sample.ip >= kernelMod.base &&
                    sample.ip < kernelMod.base + kernelMod.size)
                    type =  modKernel;
                else
                    type = modOther;
            }
            else
            {
                modMutex.lock();
                modmap_t::iterator it = (*m).lower_bound(sample.ip);
                if (it == (*m).end())
                {
                    if (sample.ip >= kernelMod.base &&
                        sample.ip < kernelMod.base + kernelMod.size)
                        type = modKernel;
                    else
                        it--;
                }
                if (type == modNone)
                {
                    if (sample.ip < it->first && it != (*m).begin())
                        it--;
                    if (sample.ip >= it->first &&
                        sample.ip < it->first+it->second.size)
                    {
                        type = it->second.type;
                    }
                    else
                        type = modOther;
                }
                modMutex.unlock();
            }
            assert(type != modNone);
            u.values[type]++;
#ifdef KNC
            _mm512_store_pd(&cp->counts[0], u.c);
#else
            _mm256_store_pd((double *)&cp->counts[0], u.c);
#endif
        }
        else if (ehdr.type == PERF_RECORD_MMAP)
        {
            struct {
                uint32_t    pid, tid;
                uint64_t    addr;
                uint64_t    len;
                uint64_t    pgoff;
            } smmap;
            int ret;
            int namelen = ehdr.size - sizeof(ehdr) - sizeof(smmap);;
            char filename[1024];
            assert(namelen > 0);
            ret = perf_read_buffer((perf_event_mmap_page *)ep->buf,
                (char *)&smmap, sizeof(smmap));
            assert(ret == 0);
            assert(namelen <= sizeof(filename));
            ret = perf_read_buffer((perf_event_mmap_page *)ep->buf,
                filename, namelen);
            assert(ret == 0);
            int type;
            type = modOther;
            modInfo mi;
            mi.name = filename;
            mi.type = -1;
            mi.base = smmap.addr;
            mi.size = smmap.len;
            int basepos = mi.name.find_last_of("/");
            if (basepos != std::string::npos)
            {
                std::string basename = mi.name.substr(basepos+1);
                if (basename.find("libmpi") != std::string::npos)
                    type = modMPI;
                else if (basename.find("libiomp5") != std::string::npos)
                    type = modOpenMP;
                else if (basename.find("libgomp") != std::string::npos)
                    type = modOpenMP;
                mi.type = type;
                modMutex.lock();
                modmap_t *m;
                std::map<int,modmap_t*>::iterator it = modByPid.find(smmap.pid);
                if (it == modByPid.end())
                    modByPid[smmap.pid] = new modmap_t();
                m = modByPid[smmap.pid];
                (*m)[mi.base] = mi;
                modMutex.unlock();
            }
        }
        else if (ehdr.type == PERF_RECORD_FORK)
        {
            perf_skip_buffer((perf_event_mmap_page *)ep->buf,
                ehdr.size-sizeof(ehdr));
        }
        else if (ehdr.type == PERF_RECORD_EXIT)
        {
            perf_skip_buffer((perf_event_mmap_page *)ep->buf,
                ehdr.size-sizeof(ehdr));
        }
        else
        {
            struct { uint64_t id; uint64_t lost; } lost;
            int ret = perf_read_buffer((perf_event_mmap_page *)ep->buf,
                (char *)&lost, sizeof(lost));
            assert(ret == 0);
            pthread_mutex_lock(&mutex);
            warnx("%d: lost id=%ld lost=%ld\n", cpu, lost.id, lost.lost);
            pthread_mutex_unlock(&mutex);
        }
        ep->count++;
    }
}

void
closefiles()
{
    for (int cpu = 0; cpu < gbl.ncpus; ++cpu)
    {
        //printf("%d: %lu events\n", cpu, events[cpu].count);
        close(events[cpu].fd);
    }
}

int
monitor(int pid, struct timeval *jointime)
{
    int status = -1;
    struct evinfo *ep = &events[0];
    struct pollfd fds;
    uint64_t lasttsc, t,firsttsc;
    double interval;

    pthread_barrier_wait(&barrier);
    int ret = ioctl(events[0].fd, PERF_EVENT_IOC_RESET, 0);
    if (ret) 
        err(1, "ioctl reset");
    ret = ioctl(events[0].fd, PERF_EVENT_IOC_ENABLE, 0);
    if (ret) 
        err(1, "ioctl enable");
    fds.fd = events[0].fd;
    fds.revents = 0;
    fds.events = POLLIN;
    firsttsc = lasttsc = _rdtsc();
    int iter = 1;
    for (;;)
    {
        poll(&fds, 1, 200);
        //if (fds.revents & POLLIN)
            readcounters(0);

        t = _rdtsc();
        interval = (t - lasttsc) / gbl.hz;
        // 5 times per second
        if ( (t - firsttsc) >= (uint64_t)(gbl.hz * 0.2 * iter) )
        {
            ++iter;
            lasttsc = t;
            printcounters(counters, t, interval);
        }

        if (!gbl.server)
        {
            if (waitpid(pid, &status, WNOHANG))
            {
                gettimeofday(jointime, 0);
                done = 1;
                break;
            }
        }
    }
    //close_output();
    return status;
}

static int
perf_read_buffer(struct perf_event_mmap_page *hdr, char *buf, size_t sz)
{
	size_t pgmsk = NPAGES*PAGESIZE-1;
	void *data;
	unsigned long tail;
	size_t avail_sz, m, c;
	
	/*
	 * data points to beginning of buffer payload
	 */
	data = ((void *)hdr)+PAGESIZE;

	/*
	 * position of tail within the buffer payload
	 */
	tail = hdr->data_tail & pgmsk;

	/*
	 * size of what is available
	 *
	 * data_head, data_tail never wrap around
	 */
	avail_sz = hdr->data_head - hdr->data_tail;
	if (sz > avail_sz)
		return -1;

	/*
	 * sz <= avail_sz, we can satisfy the request
	 */

	/*
	 * c = size till end of buffer
	 *
	 * buffer payload size is necessarily
	 * a power of two, so we can do:
	 */
	c = pgmsk + 1 -  tail;

	/*
	 * min with requested size
	 */
	m = c < sz ? c : sz;

	/* copy beginning */
	memcpy(buf, data+tail, m);

	/*
	 * copy wrapped around leftover
	 */
	if ((sz - m) > 0)
		memcpy(buf+m, data, sz - m);

	//printf("\nhead=%lx tail=%lx new_tail=%lx sz=%zu val 0x%lx\n", hdr->data_head, hdr->data_tail, hdr->data_tail+sz, sz, *((unsigned long *)buf));
	hdr->data_tail += sz;

	return 0;
}

static void
perf_skip_buffer(struct perf_event_mmap_page *hdr, size_t sz)
{
	if ((hdr->data_tail + sz) > hdr->data_head)
		sz = hdr->data_head - hdr->data_tail;

	hdr->data_tail += sz;
}
