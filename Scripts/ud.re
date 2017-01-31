# This file goes in /etc/irods (need sudo).  Reference to it is made in
# /etc/irods/server_config.json (need sudo) as follows:
# "re_rulebase_set": [{"filename": "ud"},{"filename":"core"}] there could be other rule sets

# -------------------- PEP Overrides ----------------- #

# Custom event hook for user dataset post processiong following an iput
acPostProcForPut {
	writeLine("serverLog", "PEP acPostProcForPut - $objPath");
	msiSplitPath($objPath, *fileDir, *fileName);
	# if a properly composed txt file is put into the landing zone, find the corresponding tarball and upack it.
	if(*fileDir == "/ebrc/workspaces/flags" && *fileName like regex "dataset_u.*_t.*[.]txt") then {
		*tarballName = trimr(*fileName,".") ++ ".tgz";
		acLandingZonePostProcForPut("/ebrc/workspaces/lz", *tarballName);
	}
	# if a file is put into the sharedWith directory of a dataset, report it
	else if(*fileDir like regex "/ebrc/workspaces/users/.*/datasets/.*/sharedWith") then {
		acSharingPostProcForPutOrDelete(*fileDir, *fileName, "grant");
	}
	else if(*fileDir like regex "/ebrc/workspaces/users/.*/externalDatasets") then {
		acExternalPostProcForPutOrDelete(*fileDir, *fileName, "create")
	}
}

# Custom event hook for determining what can be deleted 
acDataDeletePolicy {
	writeLine("serverLog", "PEP acDataDeletePolicy - $objPath");
}

# Custom event hook for user dataset post processing following the irm of a data object
# Note that this event hook responds to irm -f only.  
acPostProcForDelete {
    writeLine("serverLog", "PEP acPostProcForDelete - $objPath");
	msiSplitPath($objPath, *fileDir, *fileName);
	if(*fileDir like regex "/ebrc/workspaces/users/.*/datasets/.*/sharedWith") then {
		acSharingPostProcForPutOrDelete(*fileDir, *fileName, "revoke");
	}
	else if(*fileDir like regex "/ebrc/workspaces/users/.*/externalDatasets") then {
		acExternalPostProcForPutOrDelete(*fileDir, *fileName, "delete")
	}
}

# Custom event hook for user dataset pre-processiong preceding an irm of a collection
acPreprocForRmColl {
    writeLine("serverLog", "PEP acPreprocForRmColl - $collName");
    *results = SELECT COUNT(DATA_ID) WHERE COLL_NAME = '/ebrc/workspaces' AND DATA_NAME = 'flushMode';
	*count = 0;
	foreach(*result in *results) {
	  *count = int(*result.DATA_ID);
	  writeLine("serverLog","Count *count");
	}
    if(*count == 0 && $collName like regex "/ebrc/workspaces/users/.*/datasets/.*") {
	  acDatasetPreprocForRmColl();
	}
}

acPostProcForCreate {
  writeLine("serverLog", "PEP acPostProcForCreate - $objPath");
}

# -------------------- Supporting Actions ----------------- #

# This action is called by the acPostProcForPut action whenever a tgz file is deposited in
# the landing zone for user datasets.  It takes the collection name and the data object name
# as inputs.  The data object name is expected in the form 'dataset_u\d+_t\d+.tgz'.
# The _u\d+ portion defines the user id and the _t\d+ is a timestamp or some other
# mechanism for making the file unique.  The tarball is unpacked in /ebrc/workspaces/users/<userId>/datasets<datasetId>
# where the userId is extracted from the tarball name and the dataset id is the data id given by irods to this particular file.
acLandingZonePostProcForPut(*fileDir, *fileName) {
	*userId = int(trimr(triml(*fileName,"_u"),"_t"));  #expect file name in format dataset_u\d+_t\d+.tgz

	# insure the the user id is a positive number
	if(*userId <= 0) {
	    acFailCleanly(trimr(*fileName,"."), "IRODS acPostProcForPut", "The tarball filename, *fileName was mis-formatted.  No event was posted.");
	}

    # check user's workspace consumption and proceed only if under quota.
    writeLine("serverLog", "Checking whether user is already over quota.");
    acGetDefaultQuota(*defaultQuota);
	acGetWorkspaceUsed(*userId, *collectionSize);
	*quotaMegabytes = *defaultQuota/1000000;
	*message = "The dataset you are trying to export to EuPathDB would put you over your quota there.  Your quota there is *quotaMegabytes megabytes.";
	if(*collectionSize > *defaultQuota) {
	    acCreateCompletionFlag(trimr(*fileName,"."), *message, "failure");
	    msiGoodFailure;
	}

	# check size of user's tarball so it if, even unpacked, it will put the user's workspace over quota
	writeLine("serverLog", "Checking whether user tarball will put user over quota.")
	*tarballFile = *fileDir ++ "/" ++ *fileName;
	acGetDataObjectSize(*tarballFile, *tarballSize);
	if(*tarballSize + *collectionSize > *defaultQuota) {
	    acCreateCompletionFlag(trimr(*fileName,"."), *message, "failure");
	    msiGoodFailure;
	}

	# unpack the tarball under the user datasets folder using the data id as the dataset id.
    *userDatasetPath = "/ebrc/workspaces/users/*userId/datasets/$dataId";
	writeLine("serverLog", "Unpacking *tarballFile to *userDatasetPath");
  	msiTarFileExtract(*tarballFile, *userDatasetPath, $rescName, *UnpkStatus);

  	# Get the data needed to create an event from the dataset configuration file.
  	writeLine("serverLog", "Obtaining the dataset configuration file data.");
	acGetDatasetConfigFileContent(*userDatasetPath, *pairs)
	  
	# Add the uploaded timestamp to the dataset configuration data object belonging to the newly added dataset.
	writeLine("serverLog", "Updating the dataset configuration file data with the upload timestamp");
	acOverwriteDatasetConfigFileContent(*userDatasetPath, *pairs.modifiedContent);

  	# Assemble the line to be posted as an event and post it
  	writeLine("serverLog", "Posting the new event.");
	*content = "install\t" ++ *pairs.projects ++ "\t$dataId\t" ++ *pairs.ud_type_name ++ "\t" ++ *pairs.ud_type_version ++ "\t" ++ *pairs.owner_user_id ++ "\t" ++ *pairs.dependency ++ " " ++ *pairs.dependency_version ++ "\n";
	acPostEvent(*content);

	# Remove the tarball only if everything succeeds
	msiDataObjUnlink("objPath=*tarballFile++++replNum=0++++forceFlag=",*DelStatus);
	writeLine("serverLog", "Removed *tarballFile tarball");

	# Write out a success message
	*message = "tarball *fileName unpacked to *userDatasetPath and event posted\n";
	acCreateCompletionFlag(trimr(*fileName,"."), *message, "success")

}

# This action is called by the acPostProcForPut action whenever a data object with a name corresponding to a share
# recipient is put into a dataset's sharedWith collection or removed from it.  The action creates an event in the events
# folder with the following tab-delimited data:
# share projects user_dataset_id ud_type_name ud_type_version user_id grant or revoke
acSharingPostProcForPutOrDelete(*fileDir, *recipientId, *action) {
  msiSplitPath(*fileDir, *userDatasetPath, *trash);
  writeLine("serverLog", "User dataset is *userDatasetPath");
  msiSplitPath(*userDatasetPath, *parent, *datasetId);
  writeLine("serverLog", "Recipient is *recipientId and Dataset is *datasetId");
  
  # Fabricate a share event.
  acGetDatasetConfigFileContent(*userDatasetPath, *pairs)
  *content = "share\t" ++ *pairs.projects ++ "\t*datasetId\t" ++ *pairs.ud_type_name ++ "\t" ++ *pairs.ud_type_version ++ "\t" ++ *recipientId ++ "\t" ++ *action ++ "\n";
  acPostEvent(*content);
}

# externalDataset projects user_dataset_id ud_type_name ud_type_version user_id create/delete
acExternalPostProcForPutOrDelete(*fileDir, *fileName, *action) {
	*ownerId = trimr(*fileName,".");  # expected fileName in the form ownerId.externalDatasetId
	*externalDatasetId = triml(*fileName,".");
	writeLine("serverLog", "Owner id is *ownerId and External Dataset id is *externalDatasetId");
	
    # Fabricate an external dataset event.
	*ownerDatasetPath = "/ebrc/workspaces/users/*ownerId/datasets/*externalDatasetId"; 
    acGetDatasetConfigFileContent(*ownerDatasetPath, *pairs)
    *content = "externalDataset\t" ++ *pairs.projects ++ "\t*externalDatasetId\t" ++ *pairs.ud_type_name ++ "\t" ++ *pairs.ud_type_version ++ "\t" ++ *ownerId ++ "\t" ++ *action ++ "\n";
    acPostEvent(*content);
}

# Called before a dataset is removed.  Reads and parses the dataset.json to get the data needed to create
# an event data object.  The single line posted to the event object is composed as follows:
# content:  uninstall projects user_dataset_id ud_type_name ud_type_version
acDatasetPreprocForRmColl() {
	msiSplitPath($collName, *parent, *datasetId);
	acGetDatasetConfigFileContent($collName, *pairs);
	*content = "uninstall\t" ++ *pairs.projects ++ "\t*datasetId\t" ++ *pairs.ud_type_name ++ "\t" ++ *pairs.ud_type_version ++ "\n";
	acPostEvent(*content)
}

# Retrieves the dataset.json content in the form of key/value pairs that are digestable via the iRODS microservices.
# *userDatasetPath - input - absolute path to the dataset of interest (/ebrc/workspaces/users/<userid>/datasets/<datasetid)
# *content - output - a string containing key/value pairs
# In addition to the dataset.json content and system timestamp in milliseconds is returned to provide a unique identifier
# for any subsequent file holding this content
# TODO:  Perhaps the timestamp long int retrieval should be split off into a separate python script for separation of
# concerns.
acGetDatasetConfigFileContent(*userDatasetPath, *content) {
	*datasetConfigFile = "*userDatasetPath/dataset.json";
	acGetDataObjectSize(*datasetConfigFile, *datasetConfigFileSize);
	msiDataObjOpen("objPath=*datasetConfigFile++++replNum=0++++openFlags=O_RDONLY", *datasetConfigFileDescriptor);
	msiDataObjRead(*datasetConfigFileDescriptor, *datasetConfigFileSize, *datasetConfigData);
	# Escapes the double quotes so that the content is transmitted as an intact single string.
	*datasetConfigDataArg = execCmdArg(str(*datasetConfigData));
	msiDataObjClose(*datasetConfigFileDescriptor, *datasetConfigFileStatus);
	msiExecCmd("datasetParser.py", *datasetConfigDataArg,"null","null","null",*datasetConfigResult);
	msiGetStdoutInExecCmdOut(*datasetConfigResult, *out);
	msiString2KeyValPair(*out, *content);
}

# This rule posts the content provided to a new event data object in the events
# collection.  The use of systemTime in the event data object name may not be sufficient
# to insure uniqueness.  May need to obtain something from the caller to help insure that.
# Since iRODS microservices only return timestamps in seconds, we are resorting to a
# python call to get the timestamp in milliseconds (could have been a shell call)
acPostEvent(*eventContent) {
  msiExecCmd("produceTimestamp.py","null","null","null","null",*Result);
  msiGetStdoutInExecCmdOut(*Result,*Out);
  *fileName = "event_*Out.txt";
  *eventPath = "/ebrc/workspaces/events/*fileName";
  msiDataObjCreate(*eventPath,"null",*eventFileDescriptor);
  msiDataObjWrite(*eventFileDescriptor,*eventContent,*fileLength);
  msiDataObjClose(*eventFileDescriptor,*eventStatus);
  writeLine("serverLog", "Created event file *fileName");
  acTriggerEvent();
}

# Returns the integer size, in bytes, of the datasets currently in the user's workspace.
acGetWorkspaceUsed(*userId, *collectionSize) {
    *results =  SELECT SUM(DATA_SIZE) WHERE COLL_NAME LIKE '/ebrc/workspaces/users/*userId/datasets/%';
    *collectionSize = 0;
	foreach(*result in *results) {
	  *collectionSize = int(*result.DATA_SIZE);
	}
}

# Returns the integer default quota size in bytes.  The assumption is this file contains only a number (digits only)
# in bytes, followed by a newline.
acGetDefaultQuota(*defaultQuota) {
    *quotaFile = "/ebrc/workspaces/users/default_quota";
    acGetDataObjectSize(*quotaFile, *quotaFileSize)
    msiDataObjOpen("objPath=*quotaFile++++replNum=0++++openFlags=O_RDONLY", *quotaFileDescriptor);
	msiDataObjRead(*quotaFileDescriptor, *quotaFileSize, *quotaData);
	msiStrchop(str(*quotaData), *defaultQuota);
	msiDataObjClose(*quotaFileDescriptor, *quotaFileStatus);
	*defaultQuota = int(*defaultQuota);
}

# The dataset configuration file is re-written to include
acOverwriteDatasetConfigFileContent(*userDatasetPath, *content) {
  writeLine("serverLog", "Overwrite the original dataset configuration file with updated content.");
  *datasetConfigFile = "*userDatasetPath/dataset.json";
  msiDataObjOpen("objPath=*datasetConfigFile++++replNum=0++++openFlags=O_RDWRO_TRUNC", *datasetConfigFileDescriptor);
  msiDataObjWrite(*datasetConfigFileDescriptor, *content, *datasetConfigFileSize);
  msiDataObjClose(*datasetConfigFileDescriptor, *datasetConfigFileStatus);
}

# Fires off the RESTful call to Jenkins to process the event file.  The jobFile provides the data needed to call
# Jenkins.
acTriggerEvent() {
	*jobFile = "/ebrc/workspaces/jenkinsCommunicationConfig.txt";
	acGetDataObjectSize(*jobFile, *jobFileSize);
	msiDataObjOpen("objPath=*jobFile++++replNum=0++++openFlags=O_RDONLY", *jobFileDescriptor);
	msiDataObjRead(*jobFileDescriptor, *jobFileSize, *jobData);
    *argv = str(*jobData);
	writeLine("serverLog", "Passing *argv");
    msiExecCmd("executeJobFile.py", *argv, "null", "null", "null", *jobResult);
    msiGetStdoutInExecCmdOut(*jobResult, *out);
	writeLine("serverLog", *out);
}

# Provides a status flag, with the outcome as part of the flag file name, that provides some insight into the
# the successful and failed attempts at manipulating iRODS file data.
acCreateCompletionFlag(*identifier, *message, *outcome) {
    *statusFileName = *outcome ++ "_" ++ *identifier;
    *statusFile = "/ebrc/workspaces/flags/*statusFileName";
    msiDataObjCreate(*statusFile,"forceFlag=",*statusFileDescriptor);
    msiDataObjWrite(*statusFileDescriptor, *message, *statusFileSize);
    msiDataObjClose(*statusFileDescriptor, *statusFileStatus);
}

# Convenience method for getting the integer size in bytes of a data object
acGetDataObjectSize(*dataObject, *dataObjectSize) {
    msiSplitPath(*dataObject, *dataObjectPath, *dataObjectName);
    *results = SELECT DATA_SIZE WHERE COLL_NAME = *dataObjectPath AND DATA_NAME = *dataObjectName;
    *dataObjectSize = 0;
	foreach(*results) {
	  *dataObjectSize = int(*results.DATA_SIZE);
	}
}

# Fail cleanly - relates to non-user issues
acFailCleanly(*identifier, *subject, *message) {
  *userMessage = "The export did not proceed properly.  EuPathDB staff are looking into the issue.";
  msiSendMail("criswlawrence@gmail.com", *subject, *message);
  acCreateCompletionFlag(*identifier, *userMessage, "failure");
  msiGoodFailure;
}
