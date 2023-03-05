SHELL = /bin/sh

CC=cc
PREFIX=/usr/local
CFLAGS=-ansi -Wall -Wextra -std=c89 -pedantic -O2 -D_POSIX_C_SOURCE=200809L
LIBS=-s -lm -lcurl -lpthread
NAME=tuimarket
INCLUDES=-I/usr/local/include
LIBSPATH=-L/usr/local/lib

build: src/*
	${CC} ${CFLAGS} src/*.c -o ${NAME} ${INCLUDES} ${LIBSPATH} ${LIBS} 

install:
	cp ${NAME} ${PREFIX}/bin/
	chmod 755 ${PREFIX}/bin/${NAME}

uninstall:
	rm ${PREFIX}/bin/${NAME}

clean:
	rm -f ${NAME}
