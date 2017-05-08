

CC=gcc
LD=gcc

CC_FLAG=-g
#CC_FLAG=-O2

IPATH=-I. -I../src

TANTO=tanto

#FENCE=/usr/lib64/libefence.so.0.0
FENCE=

LIBPATH=-L/usr/lib64 
LIBS=$(LIBPATH) -lfuse  $(FENCE) 

all: $(TANTO)

.PHONY: all

#YARI_3RD_PARTY_OBJS=xxhash.o

TANTO_OBJS=tanto.o ytrace.o redislib.o 

$(TANTO): $(TANTO_OBJS)
	$(LD) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CC_FLAG) -c $< $(IPATH)

clean:
	rm -f $(TANTO_OBJS)
