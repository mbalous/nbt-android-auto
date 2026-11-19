QCC = qcc
CXXFLAGS = -V4.4.2,gcc_ntox86_cpp -lang-c++ -O2 -Wall -Wextra

.PHONY: all test
all: build/hello_nbt

build/hello_nbt: src/hello_nbt.cpp
	mkdir -p build
	$(QCC) $(CXXFLAGS) $< -o $@

# Run only in the development VM, never on the vehicle without approval.
test: all
	./build/hello_nbt
