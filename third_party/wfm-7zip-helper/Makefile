WFM_BUNDLE := CPP/7zip/Bundles/WfmHelper
PS5_BIN := wfm-7zip-helper.elf
LINUX_BIN := wfm-7zip-helper-linux

.PHONY: all debug ps5 linux clean

all: ps5 linux

debug: linux
	$(MAKE) -C $(WFM_BUNDLE) -f makefile.ps5 debug
	cp $(WFM_BUNDLE)/$(PS5_BIN) $(PS5_BIN)

ps5:
	$(MAKE) -C $(WFM_BUNDLE) -f makefile.ps5
	cp $(WFM_BUNDLE)/$(PS5_BIN) $(PS5_BIN)

linux:
	$(MAKE) -C $(WFM_BUNDLE) -f makefile.linux
	cp $(WFM_BUNDLE)/$(LINUX_BIN) $(LINUX_BIN)

clean:
	$(MAKE) -C $(WFM_BUNDLE) -f makefile.ps5 clean
	$(MAKE) -C $(WFM_BUNDLE) -f makefile.linux clean
	rm -f $(PS5_BIN) $(LINUX_BIN)
