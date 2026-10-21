# the loader the kernel's image boots with: a release of philemon, by its
# version. the image's rule clones the tag for it, vX.Y.Z, into
# obj/philemon/, and clones again only when this file names another
# release. a released tag is never moved, so the version is all there is
# to write down.
PHILEMON_VERSION := 1.0.0
PHILEMON_URL     ?= https://github.com/Onebit5/philemon.git
