#!/usr/bin/python

import json
import tarfile
import time
import os
import shutil
import sys
import requests
import tempfile
import contextlib
from requests.auth import HTTPBasicAuth
from subprocess import Popen, PIPE


class Export:
    """
    This is a generic EuPathDB export tool for Galaxy.  It is abstract and so must be subclassed by more
    specialized export tools that implement those abstract classes.
    """

    # Names for the 2 json files and the folder containing the dataset to be included in the tarball
    DATASET_JSON = "dataset.json"
    META_JSON = "meta.json"
    DATAFILES = "datafiles"

    def __init__(self, dataset_type, version, validation_script, user_id, dataset_name,
                 dataset_summary, dataset_description, tool_directory):
        """
        Initializes the export class with the parameters needed to accomplish the export of user
        datasets on Galaxy to EuPathDB projects.
        :param dataset_type: The EuPathDB type of this dataset
        :param version: The version of the EuPathDB type of this dataset
        :param validation_script: A script that handles the validation of this dataset
        :param user_id: The identity of the user on EuPathDB (the WDK user_id)
        :param dataset_name: The name the user has selected for this dataset to be exported
        :param dataset_summary: A summary of the dataset
        :param dataset_description: A fuller characterization of the dataset
        :param tool_directory: The tool directory (where validation scripts should also live
        """
        self._type = dataset_type
        self._version = version
        self._validation_script = validation_script
        self._user_id = user_id
        self._dataset_name = dataset_name
        self._summary = dataset_summary
        self._description = dataset_description
        self._tool_directory = tool_directory

        # This msec timestamp is used to denote both the created and modified times.
        self._timestamp = int(time.time() * 1000)

        # This is the name of the file to be exported sans extension.  It will be used to designate a unique temporary
        # directory and to export both the tarball and the flag that triggers IRODS to process the tarball. By
        # convention, the dataset tarball is of the form dataset_uNNNNNN_tNNNNNNN.tgz where the NNNNNN following the _u
        # is the WDK user id and _t is the msec timestamp
        self._export_file_root = 'dataset_u' + self._user_id + '_t' + str(self._timestamp)

        # Set up the configuration data
        (self._url, self._user, self._pwd) = self.collect_rest_data()

    def collect_rest_data(self):
        """
        Obtains the url and credentials needed to run the iRODS rest service.  At some point, this
        information should be fished out of a configuration file.
        :return:  A tuple containing the url, user, and password in that order
        """
        # TODO host, port, username and password will need to go into a configuration file somehow
        # TODO protocol should be SSL
        return ("http://wij.vm:8180/irods-rest/rest/fileContents/ebrc/workspaces/lz/",
                "wrkspuser",
                "passWORD")

    def validate_datasets(self):
        """
        Runs the validation script provided to the class upon initialization using the user's
        dataset files as standard input.
        :return:
        """
        dataset_files = self.identify_dataset_files()

        validation_process = Popen(['python', self._tool_directory + "/" + self._validation_script],
                                   stdin=PIPE, stdout=PIPE, stderr=PIPE)
        # output is a tuple containing (stdout, stderr)
        output = validation_process.communicate(json.dumps(dataset_files))
        if validation_process.returncode == os.EX_DATAERR:
            raise ValidationException(output[1])

    def identify_dependencies(self):
        """
        An abstract method to be addressed by a specialized export tool that furnishes a dependency json list.
        :return: The dependency json list to be returned should look as follows:
        [dependency1, dependency2, ... ]
        where each dependency is written as a json object as follows:
        {
          "resourceIdentifier": <value>,
          "resourceVersion": <value>,
          "resourceDisplayName": <value
        }
        Where no dependencies exist, an empty list is returned
        """
        raise NotImplementedError(
            "The method 'identify_dependencies(self)' needs to be implemented in the specialized export module.")

    def identify_projects(self):
        """
        An abstract method to be addressed by a specialized export tool that furnishes a EuPathDB project list.
        :return: The project list to be returned should look as follows:
        [project1, project2, ... ]
        At least one valid EuPathDB project must be listed
        """
        raise NotImplementedError(
            "The method 'identify_project(self)' needs to be implemented in the specialized export module.")

    def identify_dataset_files(self):
        """
        An abstract method to be addressed by a specialized export tool that furnishes a json list
        containing the dataset data files and the EuPath file names they must have in the tarball.
        :return: The dataset file list to be returned should look as follows:
        [dataset file1, dataset file2, ... ]
        where each dataset file is written as a json object as follows:
        {
          "name":<filename that EuPathDB expects>,
          "path":<Galaxy path to the dataset file>
        At least one valid EuPathDB dataset file must be listed
        """
        raise NotImplementedError(
            "The method 'identify_dataset_file(self)' needs to be implemented in the specialized export module.")

    def create_dataset_json_file(self, temp_path):
        """ Create and populate the dataset.json file that must be included in the tarball."""

        # Get the total size of the dataset files (needed for the json file)
        size = sum(os.stat(dataset_file['path']).st_size for dataset_file in self.identify_dataset_files())

        dataset_path = temp_path + "/" + self.DATASET_JSON
        with open(dataset_path, "w+") as json_file:
            json.dump({
              "type": {"name": self._type, "version": self._version},
              "dependencies": self.identify_dependencies(),
              "projects": self.identify_projects(),
              "owner": self._user_id,
              "size": size,
              "modified": self._timestamp,
              "created": self._timestamp
            }, json_file, indent=4)

    def create_metadata_json_file(self, temp_path):
        """" Create and populate the meta.json file that must be included in the tarball."""
        meta_path = temp_path + "/" + self.META_JSON
        with open(meta_path, "w+") as json_file:
            json.dump({"name": self._dataset_name,
                       "summary": self._summary,
                       "description": self._description
                       }, json_file, indent=4)

    def package_data_files(self, temp_path):
        """
        Copies the user's dataset files to the datafiles folder of the temporary dir and changes each
        dataset filename conferred by Galaxy to a filename expected by EuPathDB
        """
        os.mkdir(temp_path + "/" + self.DATAFILES)
        for dataset_file in self.identify_dataset_files():
            shutil.copy(dataset_file['path'], temp_path + "/" + self.DATAFILES + "/" + dataset_file['name'])

    def create_tarball(self):
        """
        Package the tarball - contains meta.json, dataset.json and a datafiles folder containing the
        user's dataset files
        """
        with tarfile.open(self._export_file_root + ".tgz", "w:gz") as tarball:
            for item in [self.META_JSON, self.DATASET_JSON, self.DATAFILES]:
                tarball.add(item)

    def process_request(self, source_file):
        """
        This method wraps the iRODS rest request into a try/catch to insure that bad responses are
        reflected back to the user.
        :param source_file: the name of the file to be uploaded to iRODS
        """
        rest_response = self.send_request(source_file)
        try:
            rest_response.raise_for_status()
        except requests.exceptions.HTTPError as e:
            print >> sys.stderr, "Error: " + str(e)
            sys.exit(os.EX_IOERR)

    def send_request(self, source_file):
        """
        This request is intended as a multi-part form post containing one file to be uploaded.  iRODS Rest
        does an iput followed by an iget, apparently.  So the response can be used to insure proper
        delivery.
        :param source_file: the name of the file to be uploaded to iRODS
        :return: the http response from an iget of the uploaded file
        """
        request = self._url + source_file
        headers = {"Accept": "application/json"}
        upload_file = {"uploadFile": open(source_file, "rb")}
        auth = HTTPBasicAuth(self._user, self._pwd)
        try:
            response = requests.post(request, auth=auth, headers=headers, files=upload_file)
        except requests.exceptions.ConnectionError as e:
            print >> sys.stderr, "Error: " + str(e)
            sys.exit(os.EX_IOERR)
        return response

    def export(self):
        """
        Does the work of exporting to EuPathDB, a tarball consisting of the user's dataset files along
        with dataset and metadata json files.
        """

        # Apply the validation first.  If it fails, exit with a data error.
        self.validate_datasets()

        orig_path = os.getcwd()

        # We need to create a temporary directory in which to assemble the tarball.
        with self.temporary_directory(self._export_file_root) as temp_path:

            # Need to temporarily work inside the temporary directory to properly construct and
            # send the tarball
            os.chdir(temp_path)

            self.package_data_files(temp_path)
            self.create_metadata_json_file(temp_path)
            self.create_dataset_json_file(temp_path)
            self.create_tarball()

            # Call the iRODS rest service to drop the tarball into the iRODS workspace landing zone
            self.process_request(self._export_file_root + ".tgz")

            # Create a empty (flag) file corresponding to the tarball
            open(self._export_file_root + ".txt", "w").close()

            # Call the iRODS rest service to drop a flag into the IRODS workspace landing zone.  This flag
            # triggers the iRODS PEP that unpacks the tarball and posts the event to Jenkins
            self.process_request(self._export_file_root + ".txt")

            print "Your dataset has been successfully exported to EuPathDB\n"
            print "Please visit an appropriate EuPathDB site to view your dataset."

            # We exit the temporary directory prior to removing it.
            os.chdir(orig_path)

    @contextlib.contextmanager
    def temporary_directory(self, dir_name):
        """
        This method creates a temporary directory such that removal is assured once the
        program completes.
        :param dir_name: The name of the temporary directory
        :return: The full path to the temporary directory
        """
        temp_path = tempfile.mkdtemp(dir_name)
        try:
            yield temp_path
        finally:
            shutil.rmtree(temp_path)


class ValidationException(Exception):
    """
    This represents the exception reported when a call to a validation script returns a data error
    (sys exit 65)
    """
    pass