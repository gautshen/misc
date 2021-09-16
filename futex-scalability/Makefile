BIN=futex-scalability
all: ${BIN}

%: %.c 
	cc -o $@ $^ -lpthread

clean:
	rm ${BIN}
