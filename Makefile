IDIR =../include
CC=gcc
CFLAGS=-I. -I$(IDIR) -Wall -Werror

ODIR=.
LDIR =../lib

LIBS=-lm  `pkg-config --cflags --libs gstreamer-1.0`

_DEPS =
DEPS = $(patsubst %,$(IDIR)/%,$(_DEPS))

_OBJ = audio.o
OBJ = $(patsubst %,$(ODIR)/%,$(_OBJ))


$(ODIR)/%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS) $(LIBS)

audio_player: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)

all: audio_player

.PHONY: clean

clean:
	rm -f $(ODIR)/*.o *~ core $(INCDIR)/*~
	rm audio_player
