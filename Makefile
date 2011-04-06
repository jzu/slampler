#
# Sorry, no install yet
#

all: slampler datamount

slampler: slampler.c
	gcc -Wall -g -lasound -lpthread -o $@ $<

datamount: datamount.c
	gcc -Wall -g -o $@ $<
