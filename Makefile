GTK != pkg-config --cflags --libs gtk+-3.0
PREFIX ?= /usr/local

.PHONY:  all install uninstall

all: lib/libqcmd.so

lib/libqcmd.so: src/qcmd.o
	gcc -shared -o $@ $<

src/qcmd.o: src/qcmd.c include/qcmd.h
	gcc -c -fPIC src/qcmd.c -o $@ -Iinclude


install: all
	install -d ${DESTDIR}${PREFIX}/lib/pkgconfig
	install -m 644 lib/libqcmd.so $(DESTDIR)${PREFIX}/lib
	install -m 644 qcmd.pc $(DESTDIR)${PREFIX}/lib/pkgconfig
	install -d ${DESTDIR}${PREFIX}/include
	install -m 644 include/*.h $(DESTDIR)${PREFIX}/include

uninstall:
	rm -f $(DESTDIR)${PREFIX}/lib/libqcmd.so \
		$(DESTDIR)${PREFIX}/lib/pkgconfig/qcmd.pc \
		$(DESTDIR)${PREFIX}/include/qcmd.h
