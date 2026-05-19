CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
LIBS     := $(shell sdl2-config --cflags --libs) -lSDL2_ttf

TARGET   := taskman
SRC      := main.cpp

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS)

run: all
	./$(TARGET)

clean:
	rm -f $(TARGET)
