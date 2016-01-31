CC := gcc
CFLAGS := -I/opt/vc/include -I/opt/vc/include/interface/vcos/pthreads/ -I/opt/vc/include/interface/vmcs_host/linux/ -pipe -Wall -Wextra -O2
LDLIBS := -L/opt/vc/lib -lbcm_host -ljpeg
RM := rm -f

all: input_dispmanx.so

input_dispmanx.so: input_dispmanx.c
	$(CC) $(CFLAGS) $^ $(LDLIBS) -shared -o "$@"

.PHONY: clean
clean:
	$(RM) input_dispmanx.so
