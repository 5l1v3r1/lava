import json
import os

class Project:
    """ Simple getter/setter class so we can support .get like a json file"""
    def __init__(self, data):
        self.values = data

    def __getitem__(self, key):
        return self.values[key]

    def __setitem__(self, key, value):
        self.values[key] = value

    def __contains__(self, key):
        return key in self.values

    def get(self, field, default):
        if field in self.values:
            return self.values[field]
        else:
            return default

def validate_host(host):
    # Path to configs
    assert 'config_dir' in host
    # path to qemu exec (correct guest)
    assert 'qemu' in host

def validate_project(project):
    # name of project
    assert 'name' in project
    # command line to run the target program (already instrumented with taint and attack queries)
    assert 'command' in project
    # path to guest qcow
    assert 'qcow' in project
    # name of snapshot from which to revert which will be booted & logged in as root
    assert 'snapshot' in project
    # path to tarfile for target (original source)
    assert 'tarfile' in project
    # namespace in db for prospective bugs
    assert 'db' in project

def parse_vars(host_json, project_name):
    with open(host_json, 'r') as host_f:
        host = json.load(host_f)

    try:
        validate_host(host)
    except AssertionError as e:
        print("Your host.json file is missing a required field")
        raise

    config_path = "{0}/{1}/{1}.json".format(host['config_dir'], project_name)
    if not os.path.isfile(config_path):
        raise RuntimeError("Could not find project config file at {}".format(config_path))

    with open(config_path, 'r') as host_f:
        project = json.load(host_f)

    try:
        validate_project(project)
    except AssertionError as e:
        print("Your project config file is missing a required field:\n{}".format(e))
        raise
        
    for field, prefix in [("tarfile", "tar_dir"), ("qcow", "qcow_dir")]:
        project[field] = host[prefix]+"/"+project[field]

    for field in ["inputs"]:
        if field not in project.keys(): continue
        target_val = []
        for inp in project["inputs"]:
            target_val.append("{config_dir}/{name}/{field}".format(config_dir=host["config_dir"],
                                name=project["name"], field=inp))
        project["inputs"] = target_val

    for field in ["injfixupsscript", "fixupsscript"]:
        if field not in project.keys(): continue
        project[field] = ("{config_dir}/{name}/{field}".format(config_dir=host["config_dir"],
                            name=project["name"], field=project[field]))

    project["qemu"] = host["qemu"]
    print("Host outputdir={}".format(host["output_dir"]))
    project["output_dir"] = host["output_dir"] + "/" + project["name"]
    project["directory"] = host["output_dir"]

    project["config_dir"] = host["config_dir"]+"/" + project["name"]

    return Project(project)

if __name__ == '__main__':
    # Basic test
    import sys
    import pprint
    project = parse_vars(sys.argv[1], sys.argv[2])
    pprint.pprint(project.values)
    project["foo"] = "good_fake_val"
    assert "good" in (project.get('fake', 'good_fake_val'))
    assert "good" in (project.get('foo', 'bad_fake_val'))
    assert "bad" not in (project.get('qemu', 'bad_fake_val'))
