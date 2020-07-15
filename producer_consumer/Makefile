BIN=producer_consumer
all: ${BIN}

%: %.c 
	cc -o $@ $^ -lpthread

clean:
	rm ${BIN}
