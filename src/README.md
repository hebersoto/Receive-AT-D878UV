# Source

The decoder is `afsk.c` (AFSK1200 demodulator + AX.25 deframer; host-testable with
`make test`). `target.c` is the on-radio glue and pop-up drawer, `wrap.S` the exception
wrappers, and `build_image.py` patches a stock D878UV 4.00 image to add this code.

You need a legally obtained stock **D878UV 4.00** firmware set (`.CDD`/`.CDI`/`.spi`)
from AnyTone to build; this project does not redistribute the unmodified vendor firmware.

```
make                 # builds hook.bin with arm-none-eabi-gcc
make test            # host self-test of the decoder
python3 build_image.py "<stock 4.00 folder>" hook.elf hook.bin <out_dir>
```
