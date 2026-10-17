# Compiler and Flags
CXX = g++

# 1. ADD -DTAGLIB_STATIC HERE
CXXFLAGS = -I. -DSFML_STATIC -DIMGUI_SFML_STATIC -DTAGLIB_STATIC -O2 -ltag

# 2. ENSURE -ltag AND -lz ARE LINKED AT THE END
LDFLAGS = -static -static-libgcc -static-libstdc++ \
          -lsfml-graphics-s -lsfml-window-s -lsfml-audio-s -lsfml-system-s \
          -lopenal -lflac -lvorbisenc -lvorbisfile -lvorbis -logg \
          -lopengl32 -lole32 -luuid -lfreetype -lbz2 -lharfbuzz -lgraphite2 -lrpcrt4 -ldwrite \
	  -lwinmm -lgdi32 -luser32 -lkernel32 -lavrt -lpng -lbrotlidec -lbrotlicommon \
          -ltag -lz -mwindows -lwinhttp
		  
# Source Files
SRCS = mishplay.cpp visualizer.cpp imgui.cpp imgui_draw.cpp imgui_widgets.cpp imgui_tables.cpp imgui-SFML.cpp platform-win.cpp discordrpc.cpp nowplaying_popup.cpp
OBJS = $(SRCS:.cpp=.o) resource.o

# Output Directory
BUILD_DIR = builds

# Standard MSYS2/Unix-style Date and Time (No \r carriage returns!)
DATE = $(shell date +%m%d%Y)
TIME = $(shell date +%I%M%p | tr 'A-Z' 'a-z')
TARGET = $(BUILD_DIR)/mishplayer-$(DATE)-$(TIME).exe

.PHONY: all build clean setup

# Default target
all: build

# Main build target
build: setup $(TARGET)
	@echo "--------------------------------------------------"
	@echo "Successfully compiled: $(TARGET)"
	@echo "--------------------------------------------------"

# Link the final executable
$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $(TARGET) $(LDFLAGS)

# Compile C++ source files to Object files (.o)
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile Windows Resource file (Icon)
resource.o: resource.rc
	windres resource.rc -O coff -o resource.o

# Ensure the builds directory exists
setup:
	@mkdir -p $(BUILD_DIR)

# Clean up all intermediary object files
clean:
	@rm -f *.o
	@echo "Cleaned up workspace object files!"