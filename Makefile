lv8.so: lv8.cpp
	$(CC) -shared -lv8 -llua -fPIC -O4 $< -o $@
clean:
	rm -f *.so
