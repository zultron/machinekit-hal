#!/usr/bin/env python3
# -*- coding: utf-8 -*-

#####################################################################
# Description:  buildcontainerimages.py
#
#               This file, 'buildcontainerimages.py', implements functions used
#               for assembling Machinekit-HAL Docker images for building native
#               binaries
#
# Copyright (C) 2020    Jakub Fišer  <jakub DOT fiser AT eryaf DOT com>
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
#
######################################################################

"""
Script for building Docker container images for Machinekit-HAL builder system
"""

# Debian 9 Stretch, Ubuntu 18.04 Bionic and (probably) other older distributions
# need in environment LANG=C.UTF-8 (or other similar specification of encoding)
# to properly function

__license__ = "LGPL 2.1"

import argparse
import sh
import os
import sys
import json
import math
import datetime
import machinekit_hal_script_helpers as helpers


class Buildcontainerimage_script():
    def __init__(self: object, path):
        self.normalized_path = helpers.NormalizeMachinekitHALPath(path)()
        self.get_git_data()

    def get_git_data(self: object) -> None:
        self.git_sha = sh.git("rev-parse",
                              "HEAD",
                              _tty_out=False,
                              _cwd=self.normalized_path).strip()
        self.author_name = sh.git("show",
                                  "-s",
                                  "--pretty=%an",
                                  "HEAD",
                                  _tty_out=False,
                                  _cwd=self.normalized_path).strip()
        self.author_email = sh.git("show",
                                   "-s",
                                   "--format=%ae",
                                   "HEAD",
                                   _tty_out=False,
                                   _cwd=self.normalized_path).strip()
        self.git_remote_url = sh.git("ls-remote",
                                     "--get-url",
                                     _tty_out=False,
                                     _cwd=self.normalized_path).strip()
        self.git_branch = sh.git("rev-parse",
                                 "--abbrev-ref",
                                 "HEAD",
                                 _tty_out=False,
                                 _cwd=self.normalized_path).strip()

    def load_debian_distro_settings(self: object) -> None:
        json_path = "{0}/scripts/debian-distro-settings.json".format(
            self.normalized_path)
        with open(json_path, "r") as reader:
            self.debian_settings = json.load(reader)

    def can_the_combination_be_build(self: object, distribution, version, architecture) -> bool:
        for os in self.debian_settings['osVersions']:
            if (os['distributionID'].lower().__eq__(distribution.lower()) and
                (os['distributionCodename'].lower().__eq__(version.lower()) or
                 str(os['releaseNumber']).__eq__(version))):
                for combination in self.debian_settings['allowedCombinations']:
                    if (math.isclose(os['releaseNumber'], combination['osVersionNumber'], rel_tol=1e-5) and
                            combination['architecture'].lower().__eq__(architecture.lower())):
                        self.armed_docker_base_image = os['baseImage'].lower()
                        self.armed_image_architecture = combination['architecture'].lower(
                        )
                        self.armed_os_name = os['distributionID'].lower()
                        self.armed_os_version = str(os['releaseNumber'])
                        self.armed_os_codename = os['distributionCodename'].lower(
                        )
                        return True
        return False

    def check_for_docker_experimental(self: object) -> bool:
        is_experimental_cmd = sh.docker.version('-f', '{{.Server.Experimental}}')
        print(dir(is_experimental_cmd))
        return is_experimental_cmd.stdout.decode().rstrip() == 'true'

    def build_docker_image(self: object, target, designation, specific_name) -> None:
        if any(tested is None for tested in [self.armed_docker_base_image,
                                             self.armed_image_architecture,
                                             self.armed_os_version,
                                             self.armed_os_name,
                                             self.armed_os_codename]
               ):
            raise ValueError("Not all values are prepared for build.")
        if specific_name is None:
            docker_tag = self.debian_settings['imageNameRoot'].replace(
                "@DISTRIBUTION@", self.armed_os_name).replace("@TAG@", "{0}_{1}".format(
                    self.armed_image_architecture, self.armed_os_version))
        else:
            docker_tag = specific_name
        if designation is not None:
            docker_tag = "{0}/{1}".format(designation.rstrip("/"), docker_tag)
        docker_build_arguments = [
            "--build-arg", "DEBIAN_DISTRO_BASE={0}".format(
                self.armed_docker_base_image),
            "--build-arg", "HOST_ARCHITECTURE={0}".format(
                self.armed_image_architecture)]
        docker_labels = [
            "--label", "io.machinekit.machinekit-hal.name={0}".format(
                docker_tag),
            "--label", "io.machinekit.machinekit-hal.maintainer={0} <{1}>".format(
                self.author_name, self.author_email),
            "--label", "io.machinekit.machinekit-hal.description=Machinekit-HAL {0} {1} Docker image for {2} architecture.".format(
                self.armed_os_name.capitalize(), self.armed_os_codename.capitalize(), self.armed_image_architecture),
            "--label", "io.machinekit.machinekit-hal.build-date={0}".format(
                datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%SZ")),
            "--label", "io.machinekit.machinekit-hal.vcs-ref={0}".format(
                self.git_sha),
            "--label", "io.machinekit.machinekit-hal.vcs-branch={0}".format(
                self.git_branch),
            "--label", "io.machinekit.machinekit-hal.vcs-url={0}".format(
                self.git_remote_url)
        ]
        docker_parameters = [
            "--file", "{0}/scripts/containers/buildsystem/debian/Dockerfile".format(
                self.normalized_path),
            "--tag", "{0}:latest".format(docker_tag),
        ]
        if self.check_for_docker_experimental():
            # Fix super annoying output in dumb terminal
            docker_parameters.append("--progress=plain")
        if target is not None:
            docker_parameters.append(["--target", target])

        argument_list = docker_build_arguments + docker_labels + docker_parameters

        docker_build = sh.docker.bake("build")
        docker_build(*argument_list,
                     self.normalized_path,
                     _fg=True,
                     _cwd=self.normalized_path)


def main(args):
    """ Main entry point of the app """
    try:
        buildcontainerimage_script = Buildcontainerimage_script(args.path)
        buildcontainerimage_script.load_debian_distro_settings()
        if not buildcontainerimage_script.can_the_combination_be_build(
                args.distribution, args.version, args.architecture):
            raise ValueError("Wanted combination of {0} {1} {2} is not possible to be build.".format(
                args.distribution, args.version, args.architecture))
        buildcontainerimage_script.build_docker_image(
            args.target[0] if args.target is not None else None,
            args.designation,
            args.target[1] if args.target is not None else None)
        print("Container image build ran successfully to completion!")
    except ValueError as e:
        print(e)
        sys.exit(1)


if __name__ == "__main__":
    """ This is executed when run from the command line """
    parser = argparse.ArgumentParser(
        description="Build container images for Machinekit-HAL")

    # Optional argument for path to Machinekit-HAL repository
    parser.add_argument("-p",
                        "--path",
                        action=helpers.PathExistsAction,
                        dest="path",
                        default=os.getcwd(),
                        help="Path to root of Machinekit-HAL repository")
    # Mandatory argument for distribution name
    parser.add_argument("distribution",
                        metavar="DISTRIBUTION",
                        action="store",
                        help="Distribution name for which the image will be build")
    # Mandatory argument for distribution version
    parser.add_argument("version",
                        metavar="VERSION",
                        action="store",
                        help="Distribution version for which the image will be build")
    # Mandatory argument for distribution name
    parser.add_argument("architecture",
                        metavar="ARCHITECTURE",
                        action="store",
                        help="Architecture specifics for which the image will be build")
    # Optional argument for a build Docker target
    parser.add_argument("-t",
                        "--target",
                        action="store",
                        dest="target",
                        nargs=2,
                        metavar=("TARGET", "NAME"),
                        help="Specific target to build and name of the image without prefix")
    # Optional argument for a Docker image tag prefix
    parser.add_argument("-d",
                        "--designation",
                        action="store",
                        dest="designation",
                        help="Prefix to use when tagging the image")

    args = parser.parse_args()

    main(args)
