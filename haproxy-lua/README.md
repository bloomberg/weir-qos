# HAProxy

A modified version of HAProxy handles the data path, both collecting usage metrics and enforcing limits. HAProxy supports easy customisation using lua scripts, and we leverage this for part of the system (e.g for allowing users to easily change how a user identity is extracted from a request) but in some cases (e.g for enforcing bandwidth limits) we need to add some C code to haproxy itself.

Almost all of our C code lives in new files that we add to the build, and we want it to be as easy as possible to keep up with upstream HAProxy development, so we maintain our changes as a sequence of patches which get applied (at development time) on top of a configured upstream HAProxy commit.

The workflow for modifying HAProxy C code is as follows:

1. Run [activate.sh](./activate.sh) to fetch the appropriate haproxy source commit and apply all the patches.
2. Do your regular development workflow in the `haproxy-source` directory created by the script, adding commits to the git repo in that directory as normal. The one caveat to this is new files. Any new files you wish to add to the haproxy source tree should be added to the `added-files/` subdirectory instead of being added in commits to the haproxy repo. The reason for this is just that its much easier to review changes in this repo when the changes are to the files directly instead of buried in patch files. Since the overwhelming majority of our changes are in entirely new files, there should be few modifications to the `patches/` repo.
3. Run [deactivate.sh](./deactivate.sh) to extract a new sequence of patches from the `haproxy` directory
4. Commit the new patch files.

By default, activate.sh will fetch the required haproxy commit from the upstream on GitHub. If you need a commit from the haproxy.org git server, or from any other mirror, you can set the WEIR_HAPROXY_REPO_URL environment variable (if calling activate.sh directory) or CMake variable (if building via CMake) to fetch from somewhere else.


## Upgrading to a new base version of HAProxy
Upgrading to a new version of HAProxy should be relatively simple, but depends on how many conflicts the existing patches generate.
This is one of the main reasons we try to keep patches of existing files to a minimum, preferring instead to add entirely new files whenever possible.

To upgrade to a new version of HAProxy:

1. Update the version referenced in [activate.sh](./activate.sh) so that `WEIR_HAPROXY_BASE_COMMIT` is set to the tag (or commit hash) that you wish to use.
    * Note that this *may* require that you update the default `WEIR_HAPROXY_REPO_URL` as well, since upstream haproxy keeps only `x.y.0` versions in their main repository, patches are released on minor-version-specific repositories.
2. Run `activate.sh`. This may fail due to conflicts or patches failing to apply. Conflicts will need to be resolved and patches that fail to apply will need to be applied manually (run `git am --show-current-patch` from `haproxy-source` and manually apply and stage those changes before running `git am --continue`, repeat until it succeeds/completes).
3. Run `deactivate.sh` to pull out your updated patches into patch files that can sensibly be committed.
4. Test your changes (at the very least make sure it compiles and the basic bandwidth and verb test scripts show the limits being enforced as expected).
