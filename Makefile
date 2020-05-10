#
# Makefile for COMP112 Final Project
#
# Ming Yang and Michael Gold
#
# Spring 2020
#

C      = gcc
CFLAGS = -std=c11 #-g3 -Wall -Wextra 
INCLUDES =

proj: proj.o
	${C} -o $@ $^ ${INCLUDES} && rm -rf *.o

%.o: %.cpp $(shell echo *.h)
	${C} ${CFLAGS} -c $<

clean:
	rm -rf proj *.o *.dSYM