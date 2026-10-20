# 坦克动荡 Tank Trouble —— 只需 g++ 和 libX11，无需任何第三方游戏引擎
CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra
LDLIBS   ?= -lX11
SRC       = src/canvas.cpp src/game.cpp src/main.cpp
OBJ       = $(SRC:.cpp=.o)

tank: $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.cpp src/canvas.hpp src/game.hpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: tank
	./tank

demo: tank
	./tank --mode 2 --fps 60

clean:
	rm -f $(OBJ) tank

.PHONY: run demo clean
