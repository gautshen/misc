BIN=cpuidle-smt-performance
all: ${BIN}

%: %.c 
	cc -o $@ $^ -lpthread

clean:
	rm ${BIN}
