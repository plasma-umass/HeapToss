LEVEL = ../..
TOOLNAME = structs

default: $(TOOLNAME)

all:: default

clean::
	rm $(TOOLNAME)

include $(LEVEL)/Makefile.common

$(TOOLNAME): $(TOOLNAME).cpp $(PROJ_LIB)/HeapTossPass$(SHLIBEXT)
	$(LLVM_BIN)/clang++ -O1 -Xclang -load -Xclang $(PROJ_LIB)/HeapTossPass$(SHLIBEXT) -o $(TOOLNAME) $(TOOLNAME).cpp -L$(PROJ_LIB) -lheaptoss
