# Included after the normal Makefile. Keep every test object outside the core's
# ordinary object paths, and share only identically configured control objects.
SNAPSHOT_OBJECTS := $(addprefix $(SNAPSHOT_BUILD)/objects/,$(filter-out mednafen/psx/gpu.o ./mednafen/psx/gpu.o,$(OBJECTS)))
SNAPSHOT_OBJECTS += $(SNAPSHOT_BUILD)/gpu-$(SNAPSHOT_MUTANT).o

snapshot_mutant: $(SNAPSHOT_TARGET)

$(SNAPSHOT_TARGET): $(SNAPSHOT_OBJECTS)
	$(LD) $(LINKOUT)$@ $^ $(LDFLAGS) $(GL_LIB) $(LIBS)

$(SNAPSHOT_BUILD)/gpu-$(SNAPSHOT_MUTANT).o: $(SNAPSHOT_GPU_SOURCE)
	$(CC) -c $(OBJOUT)$@ $< $(CFLAGS) -I$(CURDIR)/mednafen/psx

$(SNAPSHOT_BUILD)/objects/%.o: %.c
	@mkdir -p $(@D)
	$(CC) -c $(OBJOUT)$@ $< $(CFLAGS)

$(SNAPSHOT_BUILD)/objects/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) -c $(OBJOUT)$@ $< $(CXXFLAGS)

.PHONY: snapshot_mutant

-include $(SNAPSHOT_OBJECTS:.o=.d)
