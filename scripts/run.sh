rm -f build/quarzumc
rm out.asm
rm out.o
make &&
./build/quarzumc $1 --build
