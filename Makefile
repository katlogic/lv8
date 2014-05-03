FEATURES:=-DLV8_CACHE_PERSISTENT=1 -DLV8_FS_API=1
CFLAGS:=-O0 -ggdb $(FEATURES)

lv8.so: lv8.cpp lv8.hpp lv8.h pudata/pudata.h fs.cpp
	$(CC) -shared -lv8 -llua -fPIC $(CFLAGS) $< fs.cpp -o $@
clean:
	rm -f *.so
