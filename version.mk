dir-var = $(dir $(lastword $(MAKEFILE_LIST)))
set-var = $(eval $(strip $(1)) := \
	$(or $($(strip $(1))),$(strip $(2)),$(strip $(3))))

$(call set-var, VER_BYWHOM, \
	$(shell echo "`whoami`@`uname -n` `date +'%F %T %z'`"))

define cache-vars
$(call ver-var, MICARCH, .arch, unknown, $(1))
$(call ver-var, VER_SCIF, .ver.scif, unknown, $(1))
$(call ver-var, VER_LINUX, .ver.linux, unknown, $(1))
$(call ver-var, VER_BUILD, .ver.build, unknown, $(1))
endef

ver-var = $(call set-var, \
	$(1), $(shell cat $(dir-var)$(strip $(2)) 2>/dev/null), $(3))
$(eval $(cache-vars))
ver-var = echo '$($(strip $(1)))' > $(4)/$(strip $(2))
