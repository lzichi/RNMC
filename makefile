GMC: 
	+$(MAKE) -C GMC

clean_LGMC:
	+$(MAKE) -C LGMC clean

clean-all:
	@for dir in LGMC NPMC GMC; do \
		$(MAKE) -C $$dir clean; \
	done

debug-all:
	@for dir in LGMC NPMC GMC; do \
		$(MAKE) -C $$dir debug; \
	done

make-all:
	@for dir in LGMC NPMC GMC; do \
		$(MAKE) -C $$dir; \
	done 