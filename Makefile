all: qnet

qnet: qnet.o cluquorumd_net.o ping.o
	gcc -o $@ $^ -lpthread -lcman

%.o: %.c
	gcc -c -o $@ $^ -I.

clean:
	rm -f *.o *~ qnet
