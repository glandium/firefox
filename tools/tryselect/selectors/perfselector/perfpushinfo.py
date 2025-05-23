# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.


class PerfPushInfo:
    """Used to store, and pass information about the perf try pushes."""

    def __init__(
        self,
        base_revision=None,
        new_revision=None,
        base_lando_commit_id=None,
        new_lando_commit_id=None,
        framework=None,
    ):
        self.base_revision = base_revision
        self.new_revision = new_revision
        self.base_lando_commit_id = base_lando_commit_id
        self.new_lando_commit_id = new_lando_commit_id
        self.framework = framework
        self.finished_run = False

    @property
    def base_revision(self):
        return self._base_revision

    @base_revision.setter
    def base_revision(self, base_revision):
        self._base_revision = base_revision

    @property
    def new_revision(self):
        return self._new_revision

    @new_revision.setter
    def new_revision(self, new_revision):
        self._new_revision = new_revision
        self.finished_run = True

    @property
    def base_lando_commit_id(self):
        return self._base_lando_commit_id

    @base_lando_commit_id.setter
    def base_lando_commit_id(self, base_lando_commit_id):
        self._base_lando_commit_id = base_lando_commit_id

    @property
    def new_lando_commit_id(self):
        return self._new_lando_commit_id

    @new_lando_commit_id.setter
    def new_lando_commit_id(self, new_lando_commit_id):
        self._new_lando_commit_id = new_lando_commit_id
        self.finished_run = True

    def get_perfcompare_settings(self):
        """Returns all the settings required to setup a perfcompare URL."""
        return (
            self.base_revision,
            self.new_revision,
            self.framework,
        )

    def get_perfcompare_settings_lando(self):
        """Returns all the settings required to setup a perfcompare URL using lando pushes."""
        return (
            self.base_lando_commit_id,
            self.new_lando_commit_id,
            self.framework,
        )
