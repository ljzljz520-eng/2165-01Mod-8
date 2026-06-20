CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -pedantic
TARGET = arknights_td

.PHONY: all clean run

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
