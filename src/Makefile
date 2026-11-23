CROSS=arm-none-eabi-
CF=-mcpu=cortex-m4 -mthumb -Os -ffreestanding -fno-builtin -fno-common -mfloat-abi=soft -Wall
all: hook.bin
afsk.o: afsk.c afsk.h ; $(CROSS)gcc $(CF) -c afsk.c -o afsk.o
target.o: target.c afsk.h ; $(CROSS)gcc $(CF) -c target.c -o target.o
wrap.o: wrap.S ; $(CROSS)gcc $(CF) -c wrap.S -o wrap.o
hook.elf: wrap.o target.o afsk.o link.ld ; $(CROSS)ld -T link.ld wrap.o target.o afsk.o -o hook.elf
hook.bin: hook.elf ; $(CROSS)objcopy -O binary hook.elf hook.bin
clean: ; rm -f *.o hook.elf hook.bin

# host unit test (native compiler): cc build of the demod against synthetic AX.25
test: ; cc -O2 -Wall -o /tmp/test_afsk test_afsk.c afsk.c -lm && /tmp/test_afsk
