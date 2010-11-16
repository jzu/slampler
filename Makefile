#
# Sorry, no install yet
#

slampler: slampler.c
	gcc -Wall -g -lasound -lpthread -o $@ $<
