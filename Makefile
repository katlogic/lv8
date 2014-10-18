FEATURES:=-DLV8_CACHE_PERSISTENT=1 -DLV8_BINDING=1 -DNON_POSIX=0
CFLAGS:=-O0 -ggdb $(FEATURES)

lv8.so: lv8.cpp lv8.hpp lv8.h pudata/pudata.h binding.cpp
	$(CC) -shared -Wall -lv8 -llua -fPIC $(CFLAGS) $< binding.cpp -o $@
clean:
	rm -f *.so
