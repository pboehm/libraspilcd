CC = gcc

CFLAGS  = -mtune=arm1176jzf-s -fPIC -mfpu=vfp -mfloat-abi=hard -marm -O3 -Wall
LD = ld
LDFLAGS = -lrt


OBJ = main.o bcm2835.o lcd.o raspilcd.o
BIN = raspilcd
LIB = libraspilcd.so

gpio: $(OBJ)
	$(CC) $(CFLAGS) -o $(BIN) $(OBJ) $(LDFLAGS)

lib: $(OBJ)
	$(CC) -shared -o $(LIB) $(OBJ) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

.PHONY: clean
clean:
	rm -rf $(BIN) $(OBJ)
