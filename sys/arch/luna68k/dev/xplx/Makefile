
#### xplx

xplx: xplxfirm.o

xplx.rom xplx.lst: xplx.asm
	zasm -uwy xplx.asm

xplx.inc: xplx.rom
	./cdump/cdump xplx.rom > xplx.inc

xplxfirm.o: ../xplxfirm.c xplx.inc
	${CC} ${CFLAGS} -c ../xplxfirm.c -o ${.TARGET}


.PHONY:	clean
clean:
	rm xplxfirm.o xplx.rom xplx.lst xplx.inc
