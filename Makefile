lv8.so: lv8.cpp lv8.hpp lv8.h pudata.h
	$(CC) -shared -lv8 -llua -fPIC -O0 -ggdb $< -o $@
clean:
	rm -f *.so
