# Compiler settings
LLVM_CONFIG = llvm-config-14

CXX = g++
CXXFLAGS = -std=c++17 -Wall -Iinclude $(shell $(LLVM_CONFIG) --cppflags) -MMD -MP
LDFLAGS = $(shell $(LLVM_CONFIG) --ldflags --libs all --system-libs)

# C Runtime Compiler (Standard GCC)
CC = gcc
CFLAGS = -shared -fPIC

# Directories
SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin

# --- 1. CORE COMPILER FILES ---
SRCS = src/Compiler.cpp src/Core/Lexer.cpp src/Core/Parser.cpp src/Core/Sema.cpp
OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(SRCS))
DEPS = $(OBJS:.o=.d)

TARGET = $(BIN_DIR)/quirk

# --- 2. RUNTIME LIBRARY ---
# UPDATE: List all your runtime C files here
RUNTIME_SRC = src/Runtime/runtime.c

RUNTIME_LIB = $(BIN_DIR)/runtime.so

# --- RULES ---

# Build everything
all: $(TARGET) $(RUNTIME_LIB)

# Link the Compiler Executable
$(TARGET): $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# Compile C++ Source Files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# --- NEW: Build the Runtime Shared Library ---
$(RUNTIME_LIB): $(RUNTIME_SRC)
	@mkdir -p $(BIN_DIR)
	@echo "Compiling Runtime Library..."
	$(CC) $(CFLAGS) $^ -o $@ -lgc

# Include dependency files
-include $(DEPS)

# Clean up
clean:
	rm -rf $(OBJ_DIR)
	rm -f $(TARGET) $(RUNTIME_LIB)

# Run a test (Loading the runtime)
test: all
	./$(TARGET) -r tests/maps.qk > output.ll
	lli -load=$(RUNTIME_LIB) output.ll

.PHONY: all clean test