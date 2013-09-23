/*
Copyright (c) 2009-2013, Intel Corporation
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <err.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <sys/resource.h>

#include <immintrin.h>

#include "metrics.h"

static uint64_t starttsc;

static FILE *outfile;

void
setup_output()
{
    outfile = fopen(gbl.outfile, "w");
    if (outfile == NULL)
        err(1, "create %s", gbl.outfile);
    fprintf(outfile, "Time,Duration,OpenMP,MPI,Kernel,Application\n");
    starttsc = _rdtsc();
}

struct metrics
{
    uint64_t timestamp;
    double duration;
    double topenmp, tmpi, tkernel, tother;
};

static struct summary
{
    int pcount;
    double tserial;
    double tparallel;
    double topenmp, tmpi, tkernel, tother;
} summ;


void
sample(const struct metrics *s)
{
    double timestamp = (s->timestamp - starttsc) / gbl.hz;

    fprintf(outfile, "%g,%g,%g,%g,%g,%g\n", timestamp, s->duration,
        s->topenmp,
        s->tmpi,
        s->tkernel,
        s->tother);

    /* The idea is to ignore serial time when computing averages. Call it
     * serial if <= 2 threads are doing useful work.
     */
    if (s->tother <= 2.0 / (double)samplingFreq)
        summ.tserial += s->duration;
    else
    {
        summ.pcount += 1;
        summ.tparallel += s->duration;
        summ.topenmp += s->topenmp;
        summ.tmpi += s->tmpi;
        summ.tkernel += s->tkernel;
        summ.tother += s->tother;
    }
}

void
printsummary(int pid, struct timeval *forktime, struct timeval *jointime)
{
    double elapsed;
    long utime, stime, nthreads;
    double dutime, dstime, dcputime;
    struct rusage ru;
    double topenmp, tmpi, tother, tkernel;

    elapsed = ((double)jointime->tv_sec + (double)jointime->tv_usec * 1e-6) -
              ((double)forktime->tv_sec + (double)forktime->tv_usec * 1e-6);

    getrusage(RUSAGE_CHILDREN, &ru);

    dutime = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec * 1e-6;
    dstime = (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec * 1e-6;

    printf("\n");

    printf("Usage summary\n");

    dcputime = dutime + dstime;
    printf(
    "%10.2f %-23s (%4.1f%% System, %4.1f%% User )\n",
        elapsed, "seconds Elapsed",
        (dstime / dcputime) * 100,
        (dutime / dcputime) * 100
        );

    printf("\n");

    printf(
    "%10.2f %-23s (%4.1f threads)\n",
        summ.tparallel, "seconds Parallel", dcputime/elapsed);
    if (summ.tparallel)
    {
        printf(
            "        %10.2f seconds, %4.1f%% (%6.1f threads) OpenMP\n",
                summ.topenmp,
                (summ.topenmp/dcputime)*100,
                summ.topenmp/summ.tparallel
                );
        printf(
            "        %10.2f seconds, %4.1f%% (%6.1f threads) MPI\n",
                summ.tmpi,
                (summ.tmpi/dcputime)*100,
                summ.tmpi/summ.tparallel
                );
        printf(
            "        %10.2f seconds, %4.1f%% (%6.1f threads) Kernel\n",
                summ.tkernel,
                (summ.tkernel/dcputime)*100,
                summ.tkernel/summ.tparallel
                );
        printf(
            "        %10.2f seconds, %4.1f%% (%6.1f threads) Application\n",
                summ.tother,
                (summ.tother/dcputime)*100,
                summ.tother/summ.tparallel
                );
    }


    printf(
    "%10.2f %-23s\n",
        summ.tserial, "seconds Serial");

}

/* TODO (maybe): We're maxed out at 4 counters (OpenMP, MPI, Kernel, and Other)
 * because we want to use an atomic store and SNB can only do 256 bytes.
 * Somehow would have to reorganize, or just put up with inconsistent data
 * between the collecting threads and the output.
 * Hmm. We could use a sequence number like perf does with the rdpmc pattern.
 * Would have to do this to support Nehalem and/or to add more ctrs.
 */

void
printcounters(struct counter *ctrs, uint64_t timestamp, double duration)
{
    metrics s = {0};
    uint64_t counts[4];

    counts[0] = counts[1] = counts[2] = counts[3] = 0;
    s.timestamp = timestamp;
    s.duration = duration;
    for (int cpu = 0; cpu < gbl.ncpus; ++cpu)
    {
        uint64_t delta[4];
        // volatile because another thread is changing it.
        volatile counter *p = &ctrs[cpu];

#if !defined(KNC)
        union {
            __m256d c;
            uint64_t values[4];
        } t;
#define LOAD _mm256_load_pd
#define STORE _mm256_store_pd
#else
        union {
            __m512d c;
            uint64_t values[8];
        } t;
#define LOAD _mm512_load_pd
#define STORE _mm512_store_pd
#endif

        t.c = LOAD((const double *)&p->counts[0]);
        delta[0] = t.values[0] - lastctr[cpu].counts[0];
        delta[1] = t.values[1] - lastctr[cpu].counts[1];
        delta[2] = t.values[2] - lastctr[cpu].counts[2];
        delta[3] = t.values[3] - lastctr[cpu].counts[3];
        STORE((double *)&lastctr[cpu].counts[0], t.c);

        counts[0] += delta[0];
        counts[1] += delta[1];
        counts[2] += delta[2];
        counts[3] += delta[3];
    }
    const double freq = (double)samplingFreq;
    /* This converts sample counts to time in seconds */
    s.topenmp = counts[0] / freq;
    s.tmpi = counts[1] / freq;
    s.tkernel = counts[2] / freq;
    s.tother = counts[3] / freq;

    sample(&s);
}

void
close_output()
{
    if (outfile)
        fclose(outfile);
}
