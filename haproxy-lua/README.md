# HAProxy

A modified version of HAProxy handles the data path, both collecting usage metrics and enforcing limits. HAProxy supports easy customisation using lua scripts, and we leverage this for part of the system (e.g for allowing users to easily change how a user identity is extracted from a request) but in some cases (e.g for enforcing bandwidth limits) we need to add some C code to haproxy itself.

Almost all of our C code lives in new files that we add to the build, and we want it to be as easy as possible to keep up with upstream HAProxy development, so we maintain our changes as a sequence of patches which get applied (at development time) on top of a configured upstream HAProxy commit.

The workflow for modifying HAProxy C code is as follows:

1. Run [activate.sh](./activate.sh) to fetch the appropriate haproxy source commit and apply all the patches.
2. Do your regular development workflow in the `haproxy` directory created by the script, adding commits to the git repo in that directory as normal. The one caveat to this is new files. Any new files you wish to add to the haproxy source tree should be added to the `added-files/` subdirectory instead of being added in commits to the haproxy repo. The reason for this is just that its much easier to review changes in this repo when the changes are to the files directly instead of buried in patch files. Since the overwhelming majority of our changes are in entirely new files, there should be few modifications to the `patches/` repo.
3. Run [deactivate.sh](./deactivate.sh) to extract a new sequence of patches from the `haproxy` directory
4. Commit the new patch files.

By default, activate.sh will fetch the required haproxy commit from the upstream on GitHub. If you need a commit from the haproxy.org git server, or from any other mirror, you can set the WEIR_HAPROXY_REPO_URL environment variable (if calling activate.sh directory) or CMake variable (if building via CMake) to fetch from somewhere else.
