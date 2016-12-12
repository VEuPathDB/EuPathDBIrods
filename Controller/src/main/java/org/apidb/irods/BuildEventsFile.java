package org.apidb.irods;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;

import org.gusdb.wdk.model.Utilities;
import org.gusdb.wdk.model.WdkModelException;
import org.gusdb.wdk.model.config.ModelConfig;
import org.gusdb.wdk.model.config.ModelConfigParser;
import org.gusdb.wdk.model.config.ModelConfigUserDatasetStore;
import org.gusdb.wdk.model.user.dataset.UserDatasetStore;
import org.gusdb.wdk.model.user.dataset.UserDatasetStoreAdaptor;
import org.xml.sax.SAXException;

public class BuildEventsFile {
  public static final String EVENTS_DIR = "/ebrc/workspaces/events";
	
  public static void main(String[] args) throws Exception {
	String projectId = System.getenv("PROJECT_ID");
	System.out.println("Project: " + projectId);
	String gusHome = System.getProperty(Utilities.SYSTEM_PROPERTY_GUS_HOME);     
    ModelConfigParser parser = new ModelConfigParser(gusHome);
    ModelConfig modelConfig = null;
	try {
	  modelConfig = parser.parseConfig(projectId);
	} 
	catch(WdkModelException | SAXException | IOException e) {
	  e.printStackTrace();
	  throw new RuntimeException(e);
	}
	ModelConfigUserDatasetStore udsConfig = modelConfig.getUserDatasetStoreConfig();
    UserDatasetStore uds = udsConfig.getUserDatasetStore();
	UserDatasetStoreAdaptor udsa = uds.getUserDatasetStoreAdaptor();
	List<Path> eventFiles = new ArrayList<>();
	try {
	  eventFiles = udsa.getPathsInDir(Paths.get(EVENTS_DIR));
	}
	catch (WdkModelException e) {
	  e.printStackTrace();
	  throw new RuntimeException(e);
	}
	StringBuilder events = new StringBuilder();
	for(Path eventFile : eventFiles) {
	  String eventFilename = eventFile.getFileName().toString();
	  String timestamp = eventFilename.substring(eventFilename.indexOf('_') + 1, eventFilename.indexOf('.'));
	  String event = udsa.readFileContents(eventFile);
	  events.append(timestamp + "\t" + event + System.getProperty("line.separator"));
	} 
	System.out.println(events.toString());
	String wdkTempDir = modelConfig.getWdkTempDir();
	Path eventsFile =  Paths.get(wdkTempDir + File.pathSeparator + "eventsFile");
	try {
	  Files.write(eventsFile, events.toString().getBytes());
    }
    catch (IOException e) {
      throw new WdkModelException(e);
    }
  }

}
