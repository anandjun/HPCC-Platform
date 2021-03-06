/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#pragma warning (disable : 4786)

#include "ws_packageprocessService.hpp"
#include "daclient.hpp"
#include "dalienv.hpp"
#include "dadfs.hpp"
#include "dfuutil.hpp"
#include "ws_fs.hpp"
#include "ws_workunits.hpp"
#include "packageprocess_errors.h"
#include "referencedfilelist.hpp"
#include "package.h"

#define SDS_LOCK_TIMEOUT (5*60*1000) // 5mins, 30s a bit short

void CWsPackageProcessEx::init(IPropertyTree *cfg, const char *process, const char *service)
{
}

bool CWsPackageProcessEx::onEcho(IEspContext &context, IEspEchoRequest &req, IEspEchoResponse &resp)
{
    StringBuffer respMsg;
    ISecUser* user = context.queryUser();
    if(user != NULL)
    {
        const char* name = user->getName();
        if (name && *name)
            respMsg.appendf("%s: ", name);
    }

    const char* reqMsg = req.getRequest();
    if (reqMsg && *reqMsg)
        respMsg.append(reqMsg);
    else
        respMsg.append("??");

    resp.setResponse(respMsg.str());
    return true;
}

inline StringBuffer &buildPkgSetId(StringBuffer &id, const char *process)
{
    if (!process || !*process)
        process = "*";
    return id.append("default_").append(process).replace('*', '#').replace('?', '~');
}

IPropertyTree *getPkgSetRegistry(const char *process, bool readonly)
{
    Owned<IRemoteConnection> globalLock = querySDS().connect("/PackageSets/", myProcessSession(), RTM_LOCK_WRITE|RTM_CREATE_QUERY, SDS_LOCK_TIMEOUT);
    if (!globalLock)
        throw MakeStringException(PKG_DALI_LOOKUP_ERROR, "Unable to connect to PackageSet information in dali /PackageSets");
    IPropertyTree *pkgSets = globalLock->queryRoot();
    if (!pkgSets)
        throw MakeStringException(PKG_DALI_LOOKUP_ERROR, "Unable to open PackageSet information in dali /PackageSets");

    if (!process || !*process)
        process = "*";
    StringBuffer id;
    buildPkgSetId(id, process);

    //Only lock the branch for the target we're interested in.
    VStringBuffer xpath("/PackageSets/PackageSet[@id='%s']", id.str());
    Owned<IRemoteConnection> conn = querySDS().connect(xpath.str(), myProcessSession(), readonly ? RTM_LOCK_READ : RTM_LOCK_WRITE, SDS_LOCK_TIMEOUT);
    if (!conn)
    {
        if (readonly)
            return NULL;

        Owned<IPropertyTree> pkgSet = createPTree();
        pkgSet->setProp("@id", id.str());
        pkgSet->setProp("@process", process);
        pkgSets->addPropTree("PackageSet", pkgSet.getClear());
        globalLock->commit();

        conn.setown(querySDS().connect(xpath.str(), myProcessSession(), RTM_LOCK_WRITE, SDS_LOCK_TIMEOUT));
    }

    return (conn) ? conn->getRoot() : NULL;
}

////////////////////////////////////////////////////////////////////////////////////////
const unsigned roxieQueryRoxieTimeOut = 60000;

#define SDS_LOCK_TIMEOUT (5*60*1000) // 5mins, 30s a bit short

bool isFileKnownOnCluster(const char *logicalname, const char *lookupDaliIp, IConstWUClusterInfo *clusterInfo, IUserDescriptor* userdesc)
{
    Owned<IDistributedFile> dst = queryDistributedFileDirectory().lookup(logicalname, userdesc, true);
    if (dst)
    {
        SCMStringBuffer processName;
        clusterInfo->getRoxieProcess(processName);
        if (dst->findCluster(processName.str()) != NotFound)
            return true; // file already known for this cluster
    }
    return false;
}

bool isFileKnownOnCluster(const char *logicalname, const char *lookupDaliIp, const char *target, IUserDescriptor* userdesc)
{
    Owned<IConstWUClusterInfo> clusterInfo = getTargetClusterInfo(target);
    if (!clusterInfo)
        throw MakeStringException(PKG_TARGET_NOT_DEFINED, "Could not find information about target cluster %s ", target);

    return isFileKnownOnCluster(logicalname, lookupDaliIp, clusterInfo, userdesc);
}

void cloneFileInfoToDali(StringArray &notFound, StringArray &fileNames, const char *lookupDaliIp, IConstWUClusterInfo *dstInfo, const char *srcCluster, bool overWrite, IUserDescriptor* userdesc)
{
    StringBuffer user;
    StringBuffer password;

    if (userdesc)
    {
        userdesc->getUserName(user);
        userdesc->getPassword(password);
    }

    Owned<IReferencedFileList> wufiles = createReferencedFileList(user, password);
    wufiles->addFiles(fileNames);
    SCMStringBuffer processName;
    dstInfo->getRoxieProcess(processName);
    wufiles->resolveFiles(processName.str(), lookupDaliIp, srcCluster, !overWrite, false);
    Owned<IDFUhelper> helper = createIDFUhelper();
    wufiles->cloneAllInfo(helper, overWrite, true);

    Owned<IReferencedFileIterator> iter = wufiles->getFiles();
    ForEach(*iter)
    {
        IReferencedFile &item = iter->query();
        if (item.getFlags() & (RefFileNotFound | RefFileNotOnSource))
            notFound.append(item.getLogicalName());
    }
}

void cloneFileInfoToDali(StringArray &notFound, StringArray &fileNames, const char *lookupDaliIp, const char *dstCluster, const char *srcCluster, bool overWrite, IUserDescriptor* userdesc)
{
    Owned<IConstWUClusterInfo> clusterInfo = getTargetClusterInfo(dstCluster);
    if (!clusterInfo)
        throw MakeStringException(PKG_TARGET_NOT_DEFINED, "Could not find information about target cluster %s ", dstCluster);

    cloneFileInfoToDali(notFound, fileNames, lookupDaliIp, clusterInfo, srcCluster, overWrite, userdesc);
}


void makePackageActive(IPropertyTree *pkgSetRegistry, IPropertyTree *pkgSetTree, const char *target, bool activate)
{
    if (activate)
    {
        VStringBuffer xpath("PackageMap[@querySet='%s'][@active='1']", target);
        Owned<IPropertyTreeIterator> iter = pkgSetRegistry->getElements(xpath.str());
        ForEach(*iter)
            iter->query().setPropBool("@active", false);
    }
    pkgSetTree->setPropBool("@active", activate);
}

//////////////////////////////////////////////////////////

void addPackageMapInfo(StringArray &filesNotFound, IPropertyTree *pkgSetRegistry, const char *target, const char *pmid, const char *packageSetName, const char *lookupDaliIp, const char *srcCluster, IPropertyTree *packageInfo, bool activate, bool overWrite, IUserDescriptor* userdesc)
{
    if (srcCluster && *srcCluster)
    {
        if (!isProcessCluster(lookupDaliIp, srcCluster))
            throw MakeStringException(PKG_INVALID_CLUSTER_TYPE, "Process cluster %s not found on %s DALI", srcCluster, lookupDaliIp ? lookupDaliIp : "local");
    }
    Owned<IRemoteConnection> globalLock = querySDS().connect("/PackageMaps/", myProcessSession(), RTM_LOCK_WRITE|RTM_CREATE_QUERY, SDS_LOCK_TIMEOUT);

    StringBuffer lcPmid(pmid);
    pmid = lcPmid.toLowerCase().str();

    VStringBuffer xpath("PackageMap[@id='%s']", pmid);

    IPropertyTree *pkgRegTree = pkgSetRegistry->queryPropTree(xpath.str());
    IPropertyTree *root = globalLock->queryRoot();
    IPropertyTree *mapTree = root->queryPropTree(xpath);

    if (!overWrite && (pkgRegTree || mapTree))
        throw MakeStringException(PKG_NAME_EXISTS, "Package name %s already exists, either delete it or specify overwrite", pmid);

    if (mapTree)
        root->removeTree(mapTree);

    if (pkgRegTree)
        pkgSetRegistry->removeTree(pkgRegTree);


    mapTree = root->addPropTree("PackageMap", createPTree());
    mapTree->addProp("@id", pmid);

    StringArray fileNames;
    Owned<IConstWUClusterInfo> clusterInfo = getTargetClusterInfo(target);
    if (!clusterInfo)
        throw MakeStringException(PKG_TARGET_NOT_DEFINED, "Could not find information about target cluster %s ", target);

    IPropertyTree *baseInfo = createPTree();
    Owned<IPropertyTreeIterator> iter = packageInfo->getElements("Package");
    ForEach(*iter)
    {
        IPropertyTree &item = iter->query();
        Owned<IPropertyTreeIterator> super_iter = item.getElements("SuperFile");
        if (super_iter->first())
        {
            ForEach(*super_iter)
            {
                IPropertyTree &supertree = super_iter->query();
                StringBuffer lc(supertree.queryProp("@id"));
                const char *id = lc.toLowerCase().str();
                if (*id == '~')
                    id++;
                supertree.setProp("@id", id);

                Owned<IPropertyTreeIterator> sub_iter = supertree.getElements("SubFile");
                ForEach(*sub_iter)
                {
                    IPropertyTree &subtree = sub_iter->query();
                    StringAttr subid = subtree.queryProp("@value");
                    if (subid.length())
                    {
                        if (subid[0] == '~')
                            subtree.setProp("@value", subid+1);
                        if (!isFileKnownOnCluster(subid, lookupDaliIp, clusterInfo, userdesc))
                            fileNames.append(subid);
                    }
                }
            }
            mapTree->addPropTree("Package", LINK(&item));
        }
        else
        {
            baseInfo->addPropTree("Package", LINK(&item));
        }
    }

    mergePTree(mapTree, baseInfo);
    cloneFileInfoToDali(filesNotFound, fileNames, lookupDaliIp, clusterInfo, srcCluster, overWrite, userdesc);

    globalLock->commit();

    IPropertyTree *pkgSetTree = pkgSetRegistry->addPropTree("PackageMap", createPTree("PackageMap"));
    pkgSetTree->setProp("@id", pmid);
    pkgSetTree->setProp("@querySet", target);

    makePackageActive(pkgSetRegistry, pkgSetTree, target, activate);
}

void getPackageListInfo(IPropertyTree *mapTree, IEspPackageListMapData *pkgList)
{
    pkgList->setId(mapTree->queryProp("@id"));
    pkgList->setTarget(mapTree->queryProp("@querySet"));

    Owned<IPropertyTreeIterator> iter = mapTree->getElements("Package");
    IArrayOf<IConstPackageListData> results;
    ForEach(*iter)
    {
        IPropertyTree &item = iter->query();

        Owned<IEspPackageListData> res = createPackageListData("", "");
        res->setId(item.queryProp("@id"));
        if (item.hasProp("@queries"))
            res->setQueries(item.queryProp("@queries"));
        results.append(*res.getClear());
    }
    pkgList->setPkgListData(results);
}
void getAllPackageListInfo(IPropertyTree *mapTree, StringBuffer &info)
{
    info.append("<PackageMap id='").append(mapTree->queryProp("@id")).append("'");

    Owned<IPropertyTreeIterator> iter = mapTree->getElements("Package");
    ForEach(*iter)
    {
        IPropertyTree &item = iter->query();
        info.append("<Package id='").append(item.queryProp("@id")).append("'");
        if (item.hasProp("@queries"))
            info.append(" queries='").append(item.queryProp("@queries")).append("'");
        info.append("></Package>");
    }
    info.append("</PackageMap>");
}
void listPkgInfo(const char *target, const char *process, IArrayOf<IConstPackageListMapData>* results)
{
    Owned<IRemoteConnection> globalLock = querySDS().connect("/PackageMaps/", myProcessSession(), RTM_LOCK_WRITE|RTM_CREATE_QUERY, SDS_LOCK_TIMEOUT);
    if (!globalLock)
        throw MakeStringException(PKG_DALI_LOOKUP_ERROR, "Unable to retrieve package information from dali /PackageMaps");
    IPropertyTree *root = globalLock->queryRoot();
    Owned<IPropertyTree> pkgSetRegistry = getPkgSetRegistry(process, true);
    if (!pkgSetRegistry)
        throw MakeStringException(PKG_DALI_LOOKUP_ERROR, "Unable to retrieve package information from dali for process %s", (process && *process) ? process : "*");

    StringBuffer xpath("PackageMap");
    if (target && *target)
        xpath.appendf("[@querySet='%s']", target);
    Owned<IPropertyTreeIterator> iter = pkgSetRegistry->getElements(xpath.str());
    ForEach(*iter)
    {
        IPropertyTree &item = iter->query();
        const char *id = item.queryProp("@id");
        if (id)
        {
            StringBuffer xpath;
            xpath.append("PackageMap[@id='").append(id).append("']");
            IPropertyTree *mapTree = root->queryPropTree(xpath);
            Owned<IEspPackageListMapData> res = createPackageListMapData("", "");
            res->setActive(item.getPropBool("@active"));
            getPackageListInfo(mapTree, res);
            results->append(*res.getClear());
        }
    }
}
void getPkgInfo(const char *target, const char *process, StringBuffer &info)
{
    Owned<IRemoteConnection> globalLock = querySDS().connect("/PackageMaps/", myProcessSession(), RTM_LOCK_WRITE|RTM_CREATE_QUERY, SDS_LOCK_TIMEOUT);
    if (!globalLock)
        throw MakeStringException(PKG_DALI_LOOKUP_ERROR, "Unable to retrieve package information from dali /PackageMaps");
    IPropertyTree *root = globalLock->queryRoot();
    Owned<IPropertyTree> tree = createPTree("PackageMaps");
    Owned<IPropertyTree> pkgSetRegistry = getPkgSetRegistry(process, true);
    if (!pkgSetRegistry)
        throw MakeStringException(PKG_DALI_LOOKUP_ERROR, "Unable to retrieve package information from dali for process %s", (process && *process) ? process : "*");
    StringBuffer xpath("PackageMap[@active='1']");
    if (target && *target)
        xpath.appendf("[@querySet='%s']", target);
    Owned<IPropertyTreeIterator> iter = pkgSetRegistry->getElements(xpath.str());
    ForEach(*iter)
    {
        IPropertyTree &item = iter->query();
        const char *id = item.queryProp("@id");
        if (id)
        {
            StringBuffer xpath;
            xpath.append("PackageMap[@id='").append(id).append("']");
            IPropertyTree *mapTree = root->queryPropTree(xpath);
            if (mapTree)
                mergePTree(tree, mapTree);
        }
    }

    toXML(tree, info);
}

bool deletePkgInfo(const char *name, const char *target, const char *process, bool globalScope)
{
    Owned<IRemoteConnection> pkgSetsConn = querySDS().connect("/PackageSets/", myProcessSession(), RTM_LOCK_WRITE, SDS_LOCK_TIMEOUT);
    if (!pkgSetsConn)
        throw MakeStringException(PKG_NONE_DEFINED, "No package sets defined");

    IPropertyTree* packageSets = pkgSetsConn->queryRoot();

    StringBuffer pkgSetId;
    buildPkgSetId(pkgSetId, process);
    VStringBuffer pkgSet_xpath("PackageSet[@id='%s']", pkgSetId.str());
    IPropertyTree *pkgSetRegistry = packageSets->queryPropTree(pkgSet_xpath.str());
    if (!pkgSetRegistry)
        throw MakeStringException(PKG_TARGET_NOT_DEFINED, "No package sets defined for %s", process);

    StringBuffer lcTarget(target);
    target = lcTarget.toLowerCase().str();

    StringBuffer lcName(name);
    name = lcName.toLowerCase().str();

    IPropertyTree *mapEntry = NULL;
    StringBuffer xpath;
    if (!globalScope)
    {
        xpath.appendf("PackageMap[@id='%s::%s'][@querySet='%s']", target, name, target);
        mapEntry = pkgSetRegistry->getPropTree(xpath.str());
    }
    if (!mapEntry)
    {
        xpath.clear().appendf("PackageMap[@id='%s'][@querySet='%s']", name, target);
        mapEntry = pkgSetRegistry->getPropTree(xpath.str());
        if (!mapEntry)
            throw MakeStringException(PKG_DELETE_NOT_FOUND, "Unable to delete %s - information not found", lcName.str());
    }
    StringAttr pmid(mapEntry->queryProp("@id"));
    pkgSetRegistry->removeTree(mapEntry);

    xpath.clear().appendf("PackageSet/PackageMap[@id='%s']", pmid.get());
    if (!packageSets->hasProp(xpath))
    {
        Owned<IRemoteConnection> pkgMapsConn = querySDS().connect("/PackageMaps/", myProcessSession(), RTM_LOCK_WRITE, SDS_LOCK_TIMEOUT);
        if (!pkgMapsConn)
            throw MakeStringException(PKG_DALI_LOOKUP_ERROR, "Unable to retrieve PackageMaps information from dali [/PackageMaps]");
        IPropertyTree *pkgMaps = pkgMapsConn->queryRoot();
        if (!pkgMaps)
            throw MakeStringException(PKG_DALI_LOOKUP_ERROR, "Unable to retrieve PackageMaps information from dali [/PackageMaps]");
        IPropertyTree *mapTree = pkgMaps->getPropTree(xpath.clear().appendf("PackageMap[@id='%s']", pmid.get()).str());
        if (mapTree)
            pkgMaps->removeTree(mapTree);
    }
    return true;
}

void activatePackageMapInfo(const char *target, const char *name, const char *process, bool globalScope, bool activate)
{
    if (!target || !*target)
        throw MakeStringExceptionDirect(PKG_TARGET_NOT_DEFINED, "No target defined");

    if (!name || !*name)
        throw MakeStringExceptionDirect(PKG_MISSING_PARAM, "No pmid specified");

    Owned<IRemoteConnection> globalLock = querySDS().connect("PackageSets", myProcessSession(), RTM_LOCK_WRITE|RTM_CREATE_QUERY, SDS_LOCK_TIMEOUT);
    if (!globalLock)
        throw MakeStringException(PKG_DALI_LOOKUP_ERROR, "Unable to retrieve PackageSets information from dali /PackageSets");

    StringBuffer lcTarget(target);
    target = lcTarget.toLowerCase().str();

    StringBuffer lcName(name);
    name = lcName.toLowerCase().str();

    IPropertyTree *root = globalLock->queryRoot();
    if (!root)
        throw MakeStringException(PKG_ACTIVATE_NOT_FOUND, "Unable to retrieve PackageSet information");

    StringBuffer pkgSetId;
    buildPkgSetId(pkgSetId, process);
    VStringBuffer xpath("PackageSet[@id='%s']", pkgSetId.str());
    IPropertyTree *pkgSetTree = root->queryPropTree(xpath);
    if (pkgSetTree)
    {
        IPropertyTree *mapTree = NULL;
        if (!globalScope)
        {
            xpath.clear().appendf("PackageMap[@querySet='%s'][@id='%s::%s']", target, target, name);
            mapTree = pkgSetTree->queryPropTree(xpath);
        }
        if (!mapTree)
        {
            xpath.clear().appendf("PackageMap[@querySet='%s'][@id='%s']", target, name);
            mapTree = pkgSetTree->queryPropTree(xpath);
        }
        if (!mapTree)
            throw MakeStringException(PKG_ACTIVATE_NOT_FOUND, "PackageMap %s not found on target %s", name, target);
        makePackageActive(pkgSetTree, mapTree, target, activate);
    }
}

bool CWsPackageProcessEx::onAddPackage(IEspContext &context, IEspAddPackageRequest &req, IEspAddPackageResponse &resp)
{
    resp.updateStatus().setCode(0);

    StringAttr target(req.getTarget());
    StringAttr name(req.getPackageMap());

    if (target.isEmpty())
        throw MakeStringExceptionDirect(PKG_MISSING_PARAM, "Target cluster parameter required");
    if (name.isEmpty())
        throw MakeStringExceptionDirect(PKG_MISSING_PARAM, "PackageMap name parameter required");

    StringBuffer pmid;
    if (!req.getGlobalScope())
        pmid.append(target).append("::");
    pmid.append(name.get());

    StringBuffer info(req.getInfo());
    bool activate = req.getActivate();
    bool overWrite = req.getOverWrite();
    StringAttr processName(req.getProcess());

    Owned<IUserDescriptor> userdesc;
    const char *user = context.queryUserId();
    const char *password = context.queryPassword();
    if (user && *user && *password && *password)
    {
        userdesc.setown(createUserDescriptor());
        userdesc->set(user, password);
    }

    Owned<IPropertyTree> packageTree = createPTreeFromXMLString(info.str());
    Owned<IPropertyTree> pkgSetRegistry = getPkgSetRegistry(processName.get(), false);
    StringArray filesNotFound;
    StringBuffer pkgSetId;
    buildPkgSetId(pkgSetId, processName.get());
    addPackageMapInfo(filesNotFound, pkgSetRegistry, target.get(), pmid.str(), pkgSetId.str(), req.getDaliIp(), req.getSourceProcess(), LINK(packageTree), activate, overWrite, userdesc);
    resp.setFilesNotFound(filesNotFound);

    StringBuffer msg;
    msg.append("Successfully loaded ").append(name.get());
    resp.updateStatus().setDescription(msg.str());
    return true;
}

bool CWsPackageProcessEx::onDeletePackage(IEspContext &context, IEspDeletePackageRequest &req, IEspDeletePackageResponse &resp)
{
    resp.updateStatus().setCode(0);
    StringAttr pkgMap(req.getPackageMap());
    StringAttr processName(req.getProcess());
    if (processName.length()==0)
        processName.set("*");

    bool ret = deletePkgInfo(pkgMap.get(), req.getTarget(), processName.get(), req.getGlobalScope());
    StringBuffer msg;
    (ret) ? msg.append("Successfully ") : msg.append("Unsuccessfully ");
    msg.append("deleted ").append(pkgMap.get()).append(" from ").append(req.getTarget());

    resp.updateStatus().setDescription(msg.str());
    return true;
}

bool CWsPackageProcessEx::onActivatePackage(IEspContext &context, IEspActivatePackageRequest &req, IEspActivatePackageResponse &resp)
{
    resp.updateStatus().setCode(0);
    activatePackageMapInfo(req.getTarget(), req.getPackageMap(), req.getProcess(), req.getGlobalScope(), true);
    return true;
}

bool CWsPackageProcessEx::onDeActivatePackage(IEspContext &context, IEspDeActivatePackageRequest &req, IEspDeActivatePackageResponse &resp)
{
    resp.updateStatus().setCode(0);
    activatePackageMapInfo(req.getTarget(), req.getPackageMap(), req.getProcess(), req.getGlobalScope(), false);
    return true;
}

bool CWsPackageProcessEx::onListPackage(IEspContext &context, IEspListPackageRequest &req, IEspListPackageResponse &resp)
{
    resp.updateStatus().setCode(0);
    IArrayOf<IConstPackageListMapData> results;
    StringAttr process(req.getProcess());
    listPkgInfo(req.getTarget(), process.length() ? process.get() : "*", &results);
    resp.setPkgListMapData(results);
    return true;
}

bool CWsPackageProcessEx::onGetPackage(IEspContext &context, IEspGetPackageRequest &req, IEspGetPackageResponse &resp)
{
    resp.updateStatus().setCode(0);
    StringAttr process(req.getProcess());
    StringBuffer info;
    getPkgInfo(req.getTarget(), process.length() ? process.get() : "*", info);
    resp.setInfo(info);
    return true;
}

bool CWsPackageProcessEx::onValidatePackage(IEspContext &context, IEspValidatePackageRequest &req, IEspValidatePackageResponse &resp)
{
    StringArray warnings;
    StringArray errors;
    StringArray unmatchedQueries;
    StringArray unusedPackages;
    StringArray unmatchedFiles;

    Owned<IHpccPackageSet> set;
    Owned<IPropertyTree> mapTree;

    const char *target = req.getTarget();
    if (!target || !*target)
        throw MakeStringException(PKG_TARGET_NOT_DEFINED, "Target cluster required");
    Owned<IConstWUClusterInfo> clusterInfo = getTargetClusterInfo(target);
    if (!clusterInfo)
        throw MakeStringException(PKG_TARGET_NOT_DEFINED, "Target cluster not found");
    SCMStringBuffer process;
    clusterInfo->getRoxieProcess(process);
    if (!process.length())
        throw MakeStringException(PKG_TARGET_NOT_DEFINED, "Roxie process not found");

    const char *pmid = req.getPMID();
    if (req.getActive()) //validate active map
    {
        mapTree.setown(resolveActivePackageMap(process.str(), target, true));
        if (!mapTree)
            throw MakeStringException(PKG_PACKAGEMAP_NOT_FOUND, "Active package map not found");
    }
    else if (pmid && *pmid)
    {
        mapTree.setown(getPackageMapById(req.getGlobalScope() ? NULL : target, pmid, true));
        if (!mapTree)
            throw MakeStringException(PKG_PACKAGEMAP_NOT_FOUND, "Package map %s not found", req.getPMID());
    }
    else
    {
        mapTree.setown(createPTreeFromXMLString(req.getInfo()));
        if (!mapTree)
            throw MakeStringException(PKG_LOAD_PACKAGEMAP_FAILED, "Error processing package file content");
    }

    if (req.getCheckDFS())
    {
        Owned<IReferencedFileList> pmfiles = createReferencedFileList(context.queryUserId(), context.queryPassword());
        pmfiles->addFilesFromPackageMap(mapTree);
        pmfiles->resolveFiles(process.str(), NULL, NULL, true, false);
        Owned<IReferencedFileIterator> files = pmfiles->getFiles();
        StringArray notInDFS;
        ForEach(*files)
        {
            IReferencedFile &file = files->query();
            if (file.getFlags() & RefFileNotFound)
                notInDFS.append(file.getLogicalName());
        }
        resp.updateFiles().setNotInDFS(notInDFS);
    }

    const char *id = mapTree->queryProp("@id");
    Owned<IHpccPackageMap> map = createPackageMapFromPtree(mapTree, target, id);
    if (!map)
        throw MakeStringException(PKG_LOAD_PACKAGEMAP_FAILED, "Error loading package map %s", id);

    map->validate(req.getQueryIdToVerify(), warnings, errors, unmatchedQueries, unusedPackages, unmatchedFiles);

    resp.setPMID(map->queryPackageId());
    resp.setWarnings(warnings);
    resp.setErrors(errors);
    resp.updateQueries().setUnmatched(unmatchedQueries);
    resp.updatePackages().setUnmatched(unusedPackages);
    resp.updateFiles().setUnmatched(unmatchedFiles);
    return true;
}

bool CWsPackageProcessEx::onGetQueryFileMapping(IEspContext &context, IEspGetQueryFileMappingRequest &req, IEspGetQueryFileMappingResponse &resp)
{
    const char *queryname = req.getQueryName();
    if (!queryname || !*queryname)
        throw MakeStringException(PKG_INVALID_QUERY_NAME, "Query name required");
    const char *target = req.getTarget();
    if (!target || !*target)
        throw MakeStringException(PKG_TARGET_NOT_DEFINED, "Target cluster required");

    Owned<IConstWUClusterInfo> clusterInfo = getTargetClusterInfo(target);
    if (!clusterInfo)
        throw MakeStringException(PKG_TARGET_NOT_DEFINED, "Unable to find target cluster");
    if (clusterInfo->getPlatform()!=RoxieCluster)
        throw MakeStringException(PKG_INVALID_CLUSTER_TYPE, "Roxie target required");

    Owned<IHpccPackageSet> set;
    Owned<IHpccPackageMap> ownedmap;
    const IHpccPackageMap *map = NULL;

    const char *pmid = req.getPMID();
    if (pmid && *pmid)
    {
        ownedmap.setown(createPackageMapFromPtree(getPackageMapById(req.getGlobalScope() ? NULL : target, pmid, true), target, pmid));
        if (!ownedmap)
            throw MakeStringException(PKG_LOAD_PACKAGEMAP_FAILED, "Error loading package map %s", req.getPMID());
        map = ownedmap;
    }
    else
    {
        SCMStringBuffer process;
        clusterInfo->getRoxieProcess(process);
        set.setown(createPackageSet(process.str()));
        if (!set)
            throw MakeStringException(PKG_CREATE_PACKAGESET_FAILED, "Unable to create PackageSet");
        map = set->queryActiveMap(target);
        if (!map)
            throw MakeStringException(PKG_PACKAGEMAP_NOT_FOUND, "Active package map not found");
    }
    Owned<IPropertyTree> fileInfo = createPTree();
    map->gatherFileMappingForQuery(queryname, fileInfo);

    StringArray unmappedFiles;
    Owned<IPropertyTreeIterator> it = fileInfo->getElements("File");
    ForEach(*it)
        unmappedFiles.append(it->query().queryProp(NULL));
    resp.setUnmappedFiles(unmappedFiles);

    IArrayOf<IEspSuperFile> superArray;
    it.setown(fileInfo->getElements("SuperFile"));
    ForEach(*it)
    {
        IPropertyTree &superTree = it->query();
        Owned<IEspSuperFile> superItem = createSuperFile();
        superItem->setName(superTree.queryProp("@name"));
        StringArray subArray;
        Owned<IPropertyTreeIterator> subfiles = superTree.getElements("SubFile");
        ForEach(*subfiles)
            subArray.append(subfiles->query().queryProp(NULL));
        superItem->setSubFiles(subArray);
        superArray.append(*superItem.getClear());
    }

    resp.setSuperFiles(superArray);
    return true;
}
