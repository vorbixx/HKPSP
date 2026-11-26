.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
# Read only CORE assignments, never the host makefile's targets or flags.
CORE := $(shell awk '/^CORE[[:space:]]*(:=|\+=)/ { if ($$2 == ":=") modules=""; sub(/^[^=]*=[[:space:]]*/, ""); sub(/#.*/, ""); modules=modules " " $$0 } END { print modules }' ../tools/host.mk)
ifeq ($(strip $(CORE)),)
$(error No CORE modules found in tools/host.mk)
endif
TARGET = hkpsp
OBJDIR = ../build/psp/obj
ifeq ($(DIAG),1)
ifneq ($(strip $(PERF)$(GPU_ATTRIB)$(PERF_SWEEP)),)
$(error DIAG is a separate build)
endif
TARGET = hkpsp-diag
OBJDIR = ../build/psp/obj-diag
endif
ifeq ($(PERF),1)
TARGET = hkpsp-perf
OBJDIR = ../build/psp/obj-perf
endif
ifeq ($(GPU_ATTRIB),1)
ifneq ($(PERF),)
$(error GPU_ATTRIB and PERF are separate builds)
endif
TARGET = hkpsp-gpu
OBJDIR = ../build/psp/obj-gpu
endif
# PERF_SWEEP=1: the 11-configuration CPU sweep (opt-in, package.ps1 -PerfSweep). Its own objects and ELF:
# OBJS is expanded below, so the directory must be set here; a default perf build never reuses the
# sweep's objects or link (or the reverse).
ifeq ($(PERF_SWEEP),1)
ifneq ($(PERF),1)
$(error PERF_SWEEP=1 needs PERF=1)
endif
ifeq ($(BUILD_PRX),1)
$(error PERF_SWEEP=1 builds the EBOOT only)
endif
TARGET = hkpsp-perf-sweep
OBJDIR = ../build/psp/obj-perf-sweep
endif
ifeq ($(BUILD_PRX),1)
OBJDIR := $(OBJDIR)-prx
endif
# Functional room A/B gate: use a real 16 MiB libc heap, with separate objects.
ifeq ($(ROOM_GE_HEAP_TEST),1)
OBJDIR := $(OBJDIR)-heap-test
endif
# Keep the extracted page loader explicit; the remaining adapters use the wildcard.
LOCAL := src/page_residency.cpp $(filter-out src/diag%.cpp src/page_residency.cpp src/backend_host.cpp src/main_host.cpp,$(wildcard src/*.cpp))
ifeq ($(DIAG),1)
LOCAL += $(wildcard src/diag*.cpp)
endif
# Explicit fixed-heap acceptance builds retain their own object directory.
ifneq ($(HEAP_KB),)
OBJDIR := $(OBJDIR)-heap$(HEAP_KB)
endif
OBJS := $(addprefix $(OBJDIR)/core_,$(addsuffix .o,$(sort $(CORE)))) $(patsubst src/%.cpp,$(OBJDIR)/psp_%.o,$(LOCAL))
INCDIR = ../include include
CFLAGS = -O2 -G0 -Wall -Wextra -Werror -pedantic
CXXFLAGS = -std=c++17
LIBS = -lstdc++ -lpspgu -lpsppower -lpspaudio -lm
PSP_EBOOT = EBOOT.PBP
PSP_EBOOT_SFO = PARAM.SFO
PSP_EBOOT_TITLE = HKPSP Tutorial_01
# MEMSIZE=1 is ignored on a PSP-1000 (24 MiB partition, heap 18.7 MB; ppsspp.ps1 -Psp1000). PPSSPP 1.20.4 expands for MEMSIZE=1; the current
# PSPSDK's default MEMSIZE=2 leaves this static EBOOT in the 24 MiB partition.
PSP_LARGE_MEMORY = 1
# Recorded in out/memory.txt (param_memsize) to compare with the PARAM.SFO MEMSIZE.
CFLAGS += -DHKPSP_PARAM_MEMSIZE=$(PSP_LARGE_MEMORY)
ifeq ($(ROOM_GE_HEAP_TEST),1)
CFLAGS += -DHKPSP_ROOM_GE_HEAP_TEST
endif
ifneq ($(HEAP_KB),)
CFLAGS += -DHKPSP_HEAP_KB=$(HEAP_KB)
endif
ifeq ($(DIAG),1)
CFLAGS += -DHKPSP_DIAG
PSP_EBOOT_TITLE = HKPSP diagnostics
PSP_EBOOT = ../build/psp/EBOOT-DIAG.PBP
PSP_EBOOT_SFO = ../build/psp/PARAM-DIAG.SFO
endif
ifeq ($(PERF),1)
CFLAGS += -DHKPSP_PERF
PSP_EBOOT_TITLE = HKPSP perf
PSP_EBOOT = ../build/psp/EBOOT-PERF.PBP
PSP_EBOOT_SFO = ../build/psp/PARAM-PERF.SFO
endif
ifeq ($(GPU_ATTRIB),1)
CFLAGS += -DHKPSP_PERF -DHKPSP_GPU_ATTRIB
PSP_EBOOT_TITLE = HKPSPGPU
PSP_EBOOT = ../build/psp/EBOOT-GPU.PBP
PSP_EBOOT_SFO = ../build/psp/PARAM-GPU.SFO
endif
# PERF_SWEEP=1 (see the OBJDIR block above): the sweep EBOOT, its own file.
ifeq ($(PERF_SWEEP),1)
CFLAGS += -DHKPSP_PERF_SWEEP
PSP_EBOOT = ../build/psp/EBOOT-PERF-SWEEP.PBP
PSP_EBOOT_SFO = ../build/psp/PARAM-PERF-SWEEP.SFO
endif
EXTRA_TARGETS = $(PSP_EBOOT)
# Keep relocatable links and objects separate from the unchanged static EBOOTs.
ifeq ($(BUILD_PRX),1)
PRX_OUTPUT = hkpsp.prx
ifeq ($(DIAG),1)
PRX_OUTPUT = ../build/psp/hkpsp-diag.prx
endif
ifeq ($(PERF),1)
PRX_OUTPUT = ../build/psp/hkpsp-perf.prx
endif
ifeq ($(GPU_ATTRIB),1)
PRX_OUTPUT = ../build/psp/hkpsp-gpu.prx
endif
TARGET := ../build/psp/prx/$(TARGET)
PSP_EBOOT = $(TARGET).PBP
PSP_EBOOT_SFO = $(TARGET).SFO
EXTRA_TARGETS = $(PRX_OUTPUT)
$(TARGET).elf: | ../build/psp/prx
../build/psp/prx:
	@mkdir -p $@
$(PRX_OUTPUT): $(TARGET).prx
	cp $< $@
endif
ifneq ($(filter 1,$(PERF) $(GPU_ATTRIB)),)
$(PSP_EBOOT_SFO): | ../build/psp
../build/psp:
	@mkdir -p $@
endif

$(PSP_EBOOT_SFO): Makefile
$(OBJDIR)/core_%.o: ../src/%.cpp ../tools/host.mk Makefile
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@
$(OBJDIR)/psp_%.o: src/%.cpp Makefile
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

PSPSDK := $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
-include $(OBJS:.o=.d)
