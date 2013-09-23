mkdir -p bin/intel64
mkdir -p bin/mic
mkdir -p bin/intel64/platforms
rm -f bin/intel64/overhead.bin
rm -f bin/mic/overhead.bin
cp gui/overhead-gui cli/intel64/overhead.bin bin/intel64
cp cli/scripts/overhead bin/intel64
cp cli/mic/overhead.bin bin/mic
cp cli/scripts/overhead bin/mic
cp libqt/libqxcb.so bin/intel64/platforms
