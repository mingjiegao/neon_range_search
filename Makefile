# 手动编译（无需 cmake）:
#   make -f Makefile
#
# 或者直接:
#   g++ -std=c++17 -Wall -Wextra -g -I. -o test_range_search \
#       layer_coverage.cpp historic_layer_coverage.cpp layer_map.cpp test_main.cpp

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -g -I.
TARGET = test_range_search
SRCS = layer_coverage.cpp historic_layer_coverage.cpp layer_map.cpp test_main.cpp
OBJS = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

test: $(TARGET)
	./$(TARGET)

.PHONY: all clean test
