CXX = g++
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra -pthread
TARGET = switch_latency_monitor
SRC = switch_latency_monitor.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET) *.csv *.png
