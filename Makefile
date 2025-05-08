#
# 'make'         → build both 'cruft' and 'cruftc'
# 'make cruft'   → build assembler back‑end (cruft)
# 'make cruftc'  → build C++ back‑end   (cruftc)
# 'make clean'   → remove all .o, .d, and executables
#

# compiler & flags
CXX        := g++
# make sure we can `#include "<whatever>.h"` from both include/ and src/
CXXFLAGS  := -std=c++23 -Wall -Wextra -g -Iinclude -Isrc

# directories
SRC        := src
INCLUDE    := include
OUTPUT     := output
OBJDIR     := $(OUTPUT)/obj

# find all subdirs for includes
ifeq ($(OS),Windows_NT)
  RM     := del /q /f
  MD     := mkdir
  FIX    := $(subst /,\,$1)
else
  RM     := rm -f
  MD     := mkdir -p
  FIX    := $1
endif

# collect all source files
SOURCES   := $(shell find $(SRC) -name '*.cpp')
OBJECTS   := $(patsubst $(SRC)/%.cpp,$(OBJDIR)/%.o,$(SOURCES))
DEPS      := $(OBJECTS:.o=.d)

# include paths
INCDIRS   := $(shell find $(INCLUDE) -type d)
INCLUDES  := $(patsubst %,-I%,$(INCDIRS))

.PHONY: all cruft cruftc clean run

all: cruft cruftc

cruft: $(OUTPUT) $(OUTPUT)/cruft

cruftc: $(OUTPUT) $(OUTPUT)/cruftc

# main assembler back‑end
$(OUTPUT)/cruft: $(filter-out $(OBJDIR)/codegen/cpp/CodeGenC.o,$(OBJECTS))
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^ 

# C++ emitter back‑end: include both back‑ends’ objects so main() links
$(OUTPUT)/cruftc: $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^
# compile .cpp → .o and generate .d
$(OBJDIR)/%.o: $(SRC)/%.cpp
	@$(MD) $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -MMD -MF $(@:.o=.d) -o $@

# include auto‑generated dependencies
-include $(DEPS)

# ensure OUTPUT directory exists
$(OUTPUT):
	$(MD) $(OUTPUT)

clean:
ifeq ($(OS),Windows_NT)
	@echo Deleting $(OUTPUT) directory on Windows…
	-rmdir /S /Q $(OUTPUT)
else
	@echo Deleting $(OUTPUT) directory…
	-rm -rf $(OUTPUT)
endif
	@echo "Clean complete!"

run: all
	./$(OUTPUT)/cruft
	@echo "Ran assembler back‑end."
	./$(OUTPUT)/cruftc
	@echo "Ran C++ back‑end."
