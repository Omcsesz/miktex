/**
 * @file mpc.cpp
 * @author Christian Schenk
 * @brief Creating MiKTeX packages
 *
 * @copyright Copyright © 2006-2024 Christian Schenk
 *
 * This file is part of MPC.
 *
 * MPC is licensed under GNU General Public License version 2 or any later
 * version.
 */

#define SUPPORT_LEGACY_EXTERNALNAME

#include "config.h"

#include <cstdio>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <sys/stat.h>

#include "mpc-version.h"

#include <miktex/Core/BufferSizes>
#include <miktex/Core/Cfg>
#include <miktex/Core/Directory>
#include <miktex/Core/DirectoryLister>
#include <miktex/Core/Exceptions>
#include <miktex/Core/File>
#include <miktex/Core/FileStream>
#include <miktex/Core/FileType>
#include <miktex/Core/MD5>
#include <miktex/Core/Paths>
#include <miktex/Core/Process>
#include <miktex/Core/Quoter>
#include <miktex/Core/Session>
#include <miktex/Core/TemporaryDirectory>
#include <miktex/Core/TemporaryFile>
#include <miktex/Core/less_icase_dos>
#include <miktex/PackageManager/PackageManager>
#include <miktex/Util/Tokenizer>
#include <miktex/Version>
#include <miktex/Wrappers/PoptWrapper.h>

using namespace MiKTeX::Core;
using namespace MiKTeX::Packages;
using namespace MiKTeX::Util;
using namespace MiKTeX::Wrappers;
using namespace std;

#define T_(x) MIKTEXTEXT(x)
#define Q_(x) MiKTeX::Core::Quoter<char>(x).GetData()

const std::string TheNameOfTheGame = T_("MiKTeX Package Creator");

#define PROGNAME "mpc"

enum class ArchiveFileType
{
    None,
    MSCab,
    TarBzip2,
    Zip,
    Tar,
    TarLzma
};

class PrivateKeyProvider :
    public IPrivateKeyProvider
{
public:
 
    PathName MIKTEXTHISCALL GetPrivateKeyFile() override
    {
        return privateKeyFile;
    }

    bool GetPassphrase(string& passphrase) override
    {
        passphrase = string(reinterpret_cast<const char*>(&this->passphrase[0]), this->passphrase.size());
        return true;
    }

    void SetPrivateKeyFile(const PathName& privateKeyFile)
    {
        this->privateKeyFile = privateKeyFile;
    }

    void SetPassphrase(const vector<uint8_t>& passphrase)
    {
        this->passphrase = passphrase;
    }

private:

    vector<uint8_t> passphrase;
    PathName privateKeyFile;
};

class RestoreCurrentDirectory
{

public:

    RestoreCurrentDirectory(const PathName& path)
    {
        oldPath.SetToCurrentDirectory();
        Directory::SetCurrent(path);
    }

    virtual ~RestoreCurrentDirectory()
    {
        try
        {
            Directory::SetCurrent(oldPath);
        }
        catch (const exception&)
        {
        }
    }

private:

    PathName oldPath;
};

struct PackageSpec
{
    string id; // package id
    char level = '\0';     // one of: '-', 'S', 'M', 'L', 'T'
    ArchiveFileType archiveFileType = ArchiveFileType::None;
};

typedef map<string, MD5, less_icase_dos> FileDigestTable;
  
enum Option
{
    OPT_AAA = 300,
    OPT_BUILD_TDS,
    OPT_CREATE_PACKAGE,
    OPT_DEFAULT_LEVEL,
    OPT_DISASSEMBLE_PACKAGE,
    OPT_MIKTEX_MAJOR_MINOR,
    OPT_PACKAGE_LIST,
    OPT_PASSPHRASE_FILE,
    OPT_PRIVATE_KEY_FILE,
    OPT_RELEASE_STATE,
    OPT_REPOSITORY,
    OPT_STAGING_DIR,
    OPT_STAGING_ROOTS,
    OPT_TEXMF_PARENT,
    OPT_TEXMF_PREFIX,
    OPT_TIME_PACKAGED,
    OPT_TPM_DIR,
    OPT_TPM_FILE,
    OPT_UPDATE_REPOSITORY,
    OPT_VERBOSE,
    OPT_VERSION,
};

struct MpcPackageInfo :
    public PackageInfo
{
    MD5 archiveFileDigest;
    PathName path;
    MpcPackageInfo()
    {
    }
    MpcPackageInfo(const PackageInfo& packageInfo) :
        PackageInfo(packageInfo)
    {
    }
};

class PackagedOnReversed
{

public:

    bool operator()(const MpcPackageInfo& pi1, const MpcPackageInfo& pi2) const
    {
        return pi1.timePackaged >= pi2.timePackaged;
    }
};

bool StartsWith(const string& s, const string& prefix)
{
    return s.compare(0, prefix.length(), prefix) == 0;
}

class PackageCreator :
    public IRunProcessCallback

{

public:

    void Finalize();
    void Init(const char* argv0);
    void Run(int argc, const char** argv);

    static std::string GetFileNameExtension(ArchiveFileType archiveFileType)
    {
        switch (archiveFileType)
        {
        case ArchiveFileType::MSCab:
            return MIKTEX_CABINET_FILE_SUFFIX;
        case ArchiveFileType::TarBzip2:
            return MIKTEX_TARBZIP2_FILE_SUFFIX;
        case ArchiveFileType::TarLzma:
            return MIKTEX_TARLZMA_FILE_SUFFIX;
        case ArchiveFileType::Zip:
            return MIKTEX_ZIP_FILE_SUFFIX;
        case ArchiveFileType::Tar:
            return MIKTEX_TAR_FILE_SUFFIX;
        default:
            MIKTEX_FATAL_CRT_ERROR(T_("Unknown archive file type."));
        }
    }

protected:

    void BuildTDS(const map<string, MpcPackageInfo>& packageTable, const PathName& destDir, Cfg& repositoryManifest);
    void CleanUp(const PathName& repository);
    void CollectFiles(const PathName& rootDir, const PathName& subDir, vector<string>& runFiles, size_t& sizeRunFiles, vector<string>& docFiles, size_t& sizeDocFiles, vector<string>& sourceFiles, size_t& sizeSourceFiles);
    void CollectPackage(MpcPackageInfo& packageInfo);
    void CollectPackages(const PathName& stagingRoot, map<string, MpcPackageInfo>& packageTable);
    void CollectSubTree(const PathName& path, const std::string& subDir, vector<string>& runFiles, size_t& sizeRunFiles, vector<string>& docFiles, size_t& sizeDocFiles, vector<string>& sourceFiles, size_t& sizeSourceFiles);
    void CompressArchive(const PathName& toBeCompressed, ArchiveFileType archiveFileType, const PathName& outFile);
    void CopyPackage(const MpcPackageInfo& packageinfo, const PathName& destDir);
    ArchiveFileType CreateArchiveFile(MpcPackageInfo& packageInfo, const PathName& repository, Cfg& repositoryManifest);
    void CreateFileListFile(const map<string, MpcPackageInfo>& packageTable, const PathName& repository);
    void CreateRepositoryInformationFile(const PathName& repository, Cfg& repositoryManifest, const map<string, MpcPackageInfo>& packageTable);
    void DisassemblePackage(const PathName& packageManifestFile, const PathName& sourceDir, const PathName& stagingDir);
    void DumpPackageManifests(const map<string, MpcPackageInfo>& packageTable, const PathName& path, Cfg& repositoryManifest);
    void ExecuteSystemCommand(const std::string& command, const PathName& workingDirectory);
    void Extract(const PathName& archiveFile, ArchiveFileType archiveFileType, const PathName& outDir);
    void ExtractFile(const PathName& archiveFile, ArchiveFileType archiveFileType, const PathName& toBeExtracted, const PathName& outFile);
    void FatalError(const string& s);
    PathName FindXz();
    ArchiveFileType GetDbArchiveFileType();
    std::string GetDbFileName(int id, const VersionNumber& versionNumber);
    ArchiveFileType GetPackageArchiveFileType(const MpcPackageInfo& packageInfo);
    char GetPackageLevel(const MpcPackageInfo& packageInfo) const;
    std::string GetPackageManifestsArchiveFileName();
    std::string GetRepositoryManifestArchiveFileName();
    MD5 GetTdsDigest(const FileDigestTable& fileDigests);
    std::string GetTpmArchiveFileName();
    bool HavePackageArchiveFile(const PathName& repository, const string& id, PathName& archiveFile, ArchiveFileType& archiveFileType);
    MpcPackageInfo InitializePackageInfo(const MiKTeX::Util::PathName& stagingDir);
    void InitializeStagingDirectory(const PathName& stagingDir, const PackageInfo& packageInfo, const FileDigestTable& fileDigests, const MD5& digest);
    bool IsInTeXMFDirectory(const PathName& relPath, const std::string& subDir);
    bool IsPureContainerPackage(const MpcPackageInfo& packageInfo) const;
    bool IsToBeIgnored(const MpcPackageInfo& packageInfo) const;
    map<string, MpcPackageInfo> LoadPackageManifests(const PathName& repository);
    unique_ptr<Cfg> LoadRepositoryManifest(const PathName& repository);
    void MD5CopyFiles(const vector<string>& files, const PathName& sourceDir, const std::string& sourceSubDir, const PathName& destDir, const std::string& destSubDir, FileDigestTable& fileDigests);
    MD5 MD5CopyFile(const PathName& source, const PathName& dest);
    void MD5WildCopy(const PathName& sourceTemplate, const PathName& destDir, const PathName& prefix, FileDigestTable& fileDigests);
    void ReadDescriptionFile(const MiKTeX::Util::PathName& stagingDir, string& description);
    void ReadList(const PathName& path, map<string, PackageSpec>& mapPackageList);
    void ReadList(const PathName& path, set<string>& packageList);
    void RunArchiver(ArchiveFileType archiveFileType, const PathName& archiveFile, const std::string& filter);
    void Verbose(const string& s);
    void Warning(const string& s);
    void WriteDatabase(const map<string, MpcPackageInfo>& packageTable, const PathName& repository, bool removeObsoleteSections, Cfg& repositoryManifest);
    void WriteDescriptionFile(const string& description, const PathName& stagingDir);
    void WritePackageManifestFiles(const map<string, MpcPackageInfo>& packageTable, const PathName& destDir, Cfg& repositoryManifest);
    void UpdateRepository(map<string, MpcPackageInfo>& packageTable, const PathName& repository, Cfg& repositoryManifest);

    void ExecuteSystemCommand(const std::string& command)
    {
        ExecuteSystemCommand(command, PathName());
    }

private:

    bool MIKTEXTHISCALL OnProcessOutput(const void* output, size_t n) override;

    ArchiveFileType defaultArchiveFileType = ArchiveFileType::TarLzma;
    char defaultLevel = 'T';
    VersionNumber majorMinorVersion = VersionNumber(MIKTEX_PACKAGES_MAJOR_MINOR_STR);
    bool optVerbose = false;
    map<string, PackageSpec> packageList;
    PrivateKeyProvider privateKeyProvider;
    CharBuffer<char, 512> processOutput;
    time_t programStartTime = static_cast<time_t>(-1);
    string releaseState = "stable";
    shared_ptr<Session> session;
    string texmfPrefix = "texmf";
    PathName xzExe;

    static const struct poptOption options[];
};

const struct poptOption PackageCreator::options[] =
{
    {
        "build-tds", 0, POPT_ARG_NONE | POPT_ARGFLAG_DOC_HIDDEN, 0, OPT_BUILD_TDS, T_("Create a TDS hierarchy."), nullptr
    },

    {
        "create-package", 0, POPT_ARG_NONE, 0, OPT_CREATE_PACKAGE, T_("Create a package archive file and update the database."), nullptr
    },

    {
        "default-level", 0, POPT_ARG_STRING | POPT_ARGFLAG_DOC_HIDDEN, 0, OPT_DEFAULT_LEVEL, T_("Specify the default package level (one of: S, M, L, T)."), T_("LEVEL")
    },

    {
        "disassemble-package", 0, POPT_ARG_NONE | POPT_ARGFLAG_DOC_HIDDEN, 0, OPT_DISASSEMBLE_PACKAGE, T_("Create a staging directory."), nullptr
    },

    {
        // TODO: "miktex-major-minor"
        "miktex-series", 0, POPT_ARG_STRING, 0, OPT_MIKTEX_MAJOR_MINOR, T_("Specify the MiKTeX major/minor version."), T_("MAJOR.MINOR")
    },

    {
        "package-list", 0, POPT_ARG_STRING | POPT_ARGFLAG_DOC_HIDDEN, 0, OPT_PACKAGE_LIST, T_("Specify the package list file."), T_("FILE")
    },

    {
        "passphrase-file", 0, POPT_ARG_STRING, nullptr, OPT_PASSPHRASE_FILE,
        T_("The file containing the passphrase for the private key."), T_("FILE"),
    },

    {
        "private-key-file", 0, POPT_ARG_STRING, nullptr, OPT_PRIVATE_KEY_FILE,
        T_("The private key file used for signing."), T_("FILE"),
    },

    {
        "release-state", 0, POPT_ARG_STRING, 0, OPT_RELEASE_STATE, T_("Set the release state (one of: stable, next)."), T_("STATE")
    },

    {
        "repository", 0, POPT_ARG_STRING, 0, OPT_REPOSITORY, T_("Specify the location of the local package repository."), T_("DIR")
    },

    {
        "staging-roots", 0, POPT_ARG_STRING | POPT_ARGFLAG_DOC_HIDDEN, 0, OPT_STAGING_ROOTS, T_("Specify the staging root directories."), T_("DIRS")
    },

    {
        "staging-dir", 0, POPT_ARG_STRING, 0, OPT_STAGING_DIR, T_("Specify the staging directory."), T_("DIR")
    },

    {
        "texmf-parent", 0, POPT_ARG_STRING | POPT_ARGFLAG_DOC_HIDDEN, 0, OPT_TEXMF_PARENT, T_("Specify the TEXMF parent directory."), T_("DIR")
    },

    {
        "texmf-prefix", 0, POPT_ARG_STRING | POPT_ARGFLAG_DOC_HIDDEN, 0, OPT_TEXMF_PREFIX, T_("Specify the TEXMF prefix."), T_("PREFIX")
    },

    {
        "time-packaged", 0, POPT_ARG_STRING | POPT_ARGFLAG_DOC_HIDDEN, 0, OPT_TIME_PACKAGED, T_("Specify the package time-stamp (in seconds since 1970)."), T_("TIME")
    },

    {
        "tpm-dir", 0, POPT_ARG_STRING | POPT_ARGFLAG_DOC_HIDDEN, 0, OPT_TPM_DIR, T_("Specify the destination directory for package manifest files."), T_("DIR")
    },

    {
        "tpm-file", 0, POPT_ARG_STRING | POPT_ARGFLAG_DOC_HIDDEN, 0, OPT_TPM_FILE, T_("Specify the name of a package manifest file."), T_("FILE")
    },

    {
        "update-repository", 0, POPT_ARG_NONE | POPT_ARGFLAG_DOC_HIDDEN, 0, OPT_UPDATE_REPOSITORY, T_("Update the package repository."), nullptr
    },

    {
        "verbose", 0, POPT_ARG_NONE, 0, OPT_VERBOSE, T_("Turn on verbose mode."), nullptr
    },

    {
        "version", 0, POPT_ARG_NONE, 0, OPT_VERSION, T_("Print version information and exit."), nullptr
    },

    POPT_AUTOHELP
    POPT_TABLEEND
};

void PackageCreator::Init(const char* argv0)
{
    Session::InitInfo initInfo;
    initInfo.SetProgramInvocationName(argv0);
    session = Session::Create(initInfo);
    xzExe = FindXz();
}

void PackageCreator::Finalize()
{
    session = nullptr;
}

PathName PackageCreator::FindXz()
{
    PathName xzExe;
    string path;
    if (!Utils::GetEnvironmentString("PATH", path))
    {
        FatalError(T_("PATH is not set."));
    }
    if (!session->FindFile("xz" MIKTEX_EXE_FILE_SUFFIX, path, xzExe))
    {
        FatalError(T_("The xz utility could not be found."));
    }
    return xzExe;
}

ArchiveFileType PackageCreator::GetDbArchiveFileType()
{
    if (majorMinorVersion < VersionNumber("2.7"))
    {
        return ArchiveFileType::TarBzip2;
    }
    else
    {
        return ArchiveFileType::TarLzma;
    }
}

string PackageCreator::GetDbFileName(int id, const VersionNumber& versionNumber)
{
    return fmt::format("miktex-zzdb{0}-{1}{2}", id, versionNumber.ToString(), GetFileNameExtension(GetDbArchiveFileType()));
}

string PackageCreator::GetRepositoryManifestArchiveFileName()
{
    return GetDbFileName(1, majorMinorVersion);
}

string PackageCreator::GetTpmArchiveFileName()
{
    return GetDbFileName(2, majorMinorVersion);
}

string PackageCreator::GetPackageManifestsArchiveFileName()
{
    return GetDbFileName(3, majorMinorVersion);
}

void PackageCreator::Verbose(const string& s)
{
    if (optVerbose)
    {
        cout << s << endl;
    }
}

void PackageCreator::FatalError(const string& s)
{
    cerr << PROGNAME << ": " << s << endl;
    throw 1;
}

void PackageCreator::Warning(const string& s)
{
    cerr << PROGNAME << T_(": warning: ") << s << endl;
}

MD5 PackageCreator::GetTdsDigest(const FileDigestTable& fileDigests)
{
    MD5Builder md5Builder;
    for (const pair<string, MD5>& p : fileDigests)
    {
        PathName path(p.first);
        // we must dosify the path name for backward compatibility
        path.ConvertToDos();
        md5Builder.Update(path.GetData(), path.GetLength());
        md5Builder.Update(p.second.data(), p.second.size());
    }
    return md5Builder.Final();
}

MD5 PackageCreator::MD5CopyFile(const PathName& source, const PathName& dest)
{
    FileStream fromStream(File::Open(source, FileMode::Open, FileAccess::Read, false));
    FileStream toStream(File::Open(dest, FileMode::Create, FileAccess::Write, false));
    char buf[4096];
    size_t n;
    MD5Builder md5Builder;
    while ((n = fromStream.Read(buf, 4096)) > 0)
    {
        toStream.Write(buf, n);
        md5Builder.Update(buf, n);
    }
    toStream.Close();
    fromStream.Close();
    time_t creationTime;
    time_t lastAccessTime;
    time_t lastWriteTime;
    File::GetTimes(source, creationTime, lastAccessTime, lastWriteTime);
    File::SetTimes(dest, creationTime, lastAccessTime, lastWriteTime);
    return md5Builder.Final();
}

void PackageCreator::MD5WildCopy(const PathName& sourceTemplate, const PathName& destDir, const PathName& prefix, FileDigestTable& fileDigests)
{
    PathName sourceDir(sourceTemplate);
    sourceDir.RemoveFileSpec();

    if (!Directory::Exists(sourceDir))
    {
        return;
    }

    PathName pattern(sourceTemplate);
    pattern.RemoveDirectorySpec();

    // create destination directory
    Directory::Create(destDir);

    DirectoryEntry direntry;

    unique_ptr<DirectoryLister> lister = DirectoryLister::Open(sourceDir, pattern.GetData());

    bool haveSomething = false;

    while (lister->GetNext(direntry))
    {
        haveSomething = true;

        // don't recurse
        if (direntry.isDirectory)
        {
            continue;
        }

        // path to source file
        PathName sourcePath(sourceDir / direntry.name);

        // path to destination file
        PathName destPath(destDir / direntry.name);

        // copy file and calculate its digest
        MD5 digest = MD5CopyFile(sourcePath, destPath);

        // package manifest files do not contribute to the TDS digest
        if (sourcePath.HasExtension(MIKTEX_PACKAGE_MANIFEST_FILE_SUFFIX))
        {
            continue;
        }

        // relativize file name
        const char* lpszRelPath = Utils::GetRelativizedPath(destPath.GetData(), prefix.GetData());

        // remember digest
        fileDigests[lpszRelPath] = digest;
    }

    lister->Close();
    lister = nullptr;

    if (!haveSomething)
    {
        FatalError(fmt::format(T_("No match for {0}"), Q_(sourceTemplate)));
    }
}

void PackageCreator::MD5CopyFiles(const vector<string>& files, const PathName& sourceDir, const string& sourceSubDir, const PathName& destDir, const string& destSubDir, FileDigestTable& fileDigests)
{
    // path to source root directory
    PathName sourceRootDir(sourceDir / sourceSubDir);

    // path to destination root directory
    PathName destRootDir(destDir / destSubDir);

    for (const string& fileName : files)
    {
        // source template
        PathName sourceTemplate(sourceRootDir / fileName);

        // path to destination directory
        PathName destDir(destRootDir / fileName);
        destDir.RemoveFileSpec();

        // copy file
        MD5WildCopy(sourceTemplate, destDir, destRootDir, fileDigests);
    }
}

void PackageCreator::WriteDescriptionFile(const string& description, const PathName& stagingDir)
{
    ofstream stream = File::CreateOutputStream(stagingDir / "Description");
    stream << description;
    stream.close();
}

void PackageCreator::InitializeStagingDirectory(const PathName& stagingDir, const PackageInfo& packageInfo, const FileDigestTable& fileDigests, const MD5& digest)
{
    ofstream stream;

    // write package.ini
    stream = File::CreateOutputStream(stagingDir / "package.ini");
    stream
        << "id=" << packageInfo.id << "\n"
        << "name=" << packageInfo.displayName << "\n"
        << "creator=" << packageInfo.creator << "\n"
        << "title=" << packageInfo.title << "\n"
        << "version=" << packageInfo.version << "\n"
        << "targetsystem=" << packageInfo.targetSystem << "\n"
        << "min_target_system_version=" << packageInfo.minTargetSystemVersion << "\n"
        << "md5=" << digest << "\n"
        << "ctan_path=" << packageInfo.ctanPath << "\n"
        << "copyright_owner=" << packageInfo.copyrightOwner << "\n"
        << "copyright_year=" << packageInfo.copyrightYear << "\n"
        << "license_type=" << packageInfo.licenseType << "\n";
    for (const string& p : packageInfo.requiredPackages)
    {
        stream << fmt::format("requires;={0}\n", p);
    }
    
#if defined(SUPPORT_LEGACY_EXTERNALNAME)
    stream
        << "externalname=" << packageInfo.id << "\n";
#endif
    stream.close();

    // write md5sums.txt
    stream = File::CreateOutputStream(stagingDir / "md5sums.txt");
    for (const pair<string, MD5>& p : fileDigests)
    {
        stream << p.second << " " << PathName(p.first).ToUnix() << "\n";
    }
    stream.close();

    // write Description
    if (!packageInfo.description.empty())
    {
        WriteDescriptionFile(packageInfo.description, stagingDir);
    }
}

void PackageCreator::CopyPackage(const MpcPackageInfo& packageinfo, const PathName& destDir)
{
    Verbose(fmt::format(T_("Copying {0} ..."), Q_(packageinfo.id)));

    // path to package manifest directory, e.g.:
    // /miktex/texmf/tpm/packages/
    PathName packageManifestDirectory = destDir / texmfPrefix / MIKTEX_PATH_PACKAGE_MANIFEST_DIR;

    // create package manifest directory
    Directory::Create(packageManifestDirectory);

    // create the package manifest file...
    PackageManager::WritePackageManifestFile((packageManifestDirectory / packageinfo.id).AppendExtension(MIKTEX_PACKAGE_MANIFEST_FILE_SUFFIX), packageinfo, programStartTime);

    // copy files and calculate digests
    FileDigestTable fileDigests;
    MD5CopyFiles(packageinfo.runFiles, packageinfo.path, "Files", destDir, nullptr, fileDigests);
    MD5CopyFiles(packageinfo.docFiles, packageinfo.path, "Files", destDir, nullptr, fileDigests);
    MD5CopyFiles(packageinfo.sourceFiles, packageinfo.path, "Files", destDir, nullptr, fileDigests);

    // check TDS digest
    if (!(GetTdsDigest(fileDigests) == packageinfo.digest))
    {
        FatalError(fmt::format(T_("Bad TDS digest ({0})."), packageinfo.id));
    }
}

void PackageCreator::ReadDescriptionFile(const PathName& stagingDir, string& description)
{
    PathName descriptionFileName = PathName(stagingDir) / "Description";
    if (!File::Exists(descriptionFileName))
    {
        description = "";
        return;
    }
    ifstream stream = File::CreateInputStream(descriptionFileName);
    description = string(istreambuf_iterator<char>(stream), istreambuf_iterator<char>());
    stream.close();
}

MpcPackageInfo PackageCreator::InitializePackageInfo(const PathName& stagingDir)
{
    MpcPackageInfo packageInfo;

    unique_ptr<Cfg> cfg(Cfg::Create());

    // read package.ini
    cfg->Read(PathName(stagingDir) / "package.ini");

    // get package ID (mandatory value)
    if (!cfg->TryGetValueAsString("", "id", packageInfo.id))
    {
#if defined(SUPPORT_LEGACY_EXTERNALNAME)
        if (!cfg->TryGetValueAsString("", "externalname", packageInfo.id))
        {
            FatalError(T_("Invalid package information file (id)."));
        }
#else
        FatalError(T_("Invalid package information file (id)."));
#endif
    }

    // get display name (mandatory value)
    if (!cfg->TryGetValueAsString("", "name", packageInfo.displayName))
    {
        FatalError(T_("Invalid package information file (name)."));
    }

    // get creator (optional value)
    cfg->TryGetValueAsString("", "creator", packageInfo.creator);

    // get title (optional value)
    cfg->TryGetValueAsString("", "title", packageInfo.title);

    // get version (optional value)
    cfg->TryGetValueAsString("", "version", packageInfo.version);

    // get target system (optional value)
    cfg->TryGetValueAsString("", "targetsystem", packageInfo.targetSystem);

    // get minimum required target system version (optional value)
    cfg->TryGetValueAsString("", "min_target_system_version", packageInfo.minTargetSystemVersion);

    // get required packages (optional value)
    string strReqList;
    if (cfg->TryGetValueAsString("", "requires", strReqList))
    {
        for (const string& tok : StringUtil::Split(strReqList, PathNameUtil::PathNameDelimiter))
        {
            packageInfo.requiredPackages.push_back(tok);
        }
    }

    // get TDS digest (optional value)
    string str;
    if (cfg->TryGetValueAsString("", "MD5", str))
    {
        packageInfo.digest = MD5::Parse(str);
    }

    cfg->TryGetValueAsString("", "ctan_path", packageInfo.ctanPath);
    cfg->TryGetValueAsString("", "copyright_owner", packageInfo.copyrightOwner);
    cfg->TryGetValueAsString("", "copyright_year", packageInfo.copyrightYear);
    cfg->TryGetValueAsString("", "license_type", packageInfo.licenseType);

    // read extra description file
    ReadDescriptionFile(stagingDir, packageInfo.description);

    // remember the staging directory
    packageInfo.path = stagingDir;

    return packageInfo;
}

char PackageCreator::GetPackageLevel(const MpcPackageInfo& packageInfo) const
{
    map<string, PackageSpec>::const_iterator it = packageList.find(packageInfo.id);
    if (it == packageList.end())
    {
        // assume default level, if the package is not listed
        return defaultLevel;
    }
    return it->second.level;
}

#if 0
ArchiveFileType PackageCreator::GetPackageArchiveFileType(const MpcPackageInfo& packageInfo)
{
    map<string, PackageSpec>::const_iterator it = packageList.find(packageInfo.id);
    if (it == packageList.end())
    {
        // assume default archive file type, if the package is not listed
        return defaultArchiveFileType;
    }
    return it->second.archiveFileType;
}
#endif

bool PackageCreator::IsToBeIgnored(const MpcPackageInfo& packageInfo) const
{
    return GetPackageLevel(packageInfo) == '-';
}

bool PackageCreator::IsPureContainerPackage(const MpcPackageInfo& packageInfo) const
{
    if ((packageInfo.docFiles.size() + packageInfo.sourceFiles.size()) == 0)
    {
        size_t n = packageInfo.runFiles.size();
        if (n == 0)
        {
            return true;
        }
        else if (n == 1)
        {
            return PathName(packageInfo.runFiles[0]).HasExtension(MIKTEX_PACKAGE_MANIFEST_FILE_SUFFIX);
        }
    }
    return false;
}

bool PackageCreator::IsInTeXMFDirectory(const PathName& relPath, const string& subDir)
{
    PathName texmfDirectory(texmfPrefix);
    texmfDirectory /= subDir;
    return PathName::ComparePrefixes(texmfDirectory, relPath, texmfDirectory.GetLength()) == 0;
}

void PackageCreator::CollectFiles(const PathName& rootDir, const PathName& subDir, vector<string>& runFiles, size_t& sizeRunFiles, vector<string>& docFiles, size_t& sizeDocFiles, vector<string>& sourceFiles, size_t& sizeSourceFiles)
{
    // directory to be inspected, e.g.:
    // /mypackages/a0poster/RunFiles/texmf/tex/latex/a0poster/
    PathName directory(rootDir / subDir.ToString());

    if (!Directory::Exists(directory))
    {
        return;
    }

    unique_ptr<DirectoryLister> lister = DirectoryLister::Open(directory);

    DirectoryEntry2 dirEntry;

    while (lister->GetNext(dirEntry))
    {
        // path relative to root directory
        PathName relPath(subDir);
        relPath /= dirEntry.name;

        // check to see if it is a sub-directory
        if (dirEntry.isDirectory)
        {
            // recurse into sub-directory
            CollectFiles(rootDir, relPath, runFiles, sizeRunFiles, docFiles, sizeDocFiles, sourceFiles, sizeSourceFiles);
        }
        else
        {
            // store file name
            // add file size to total size
            if (IsInTeXMFDirectory(relPath, "doc"))
            {
                docFiles.push_back(relPath.ToString());
                sizeDocFiles += dirEntry.size;
            }
            else if (IsInTeXMFDirectory(relPath, "source"))
            {
                sourceFiles.push_back(relPath.ToString());
                sizeSourceFiles += dirEntry.size;
            }
            else
            {
                runFiles.push_back(relPath.ToString());
                sizeRunFiles += dirEntry.size;
            }
        }
    }

    lister->Close();
    lister.reset();
}

void PackageCreator::CollectSubTree(const PathName& path, const string& subDir, vector<string>& runFiles, size_t& sizeRunFiles, vector<string>& docFiles, size_t& sizeDocFiles, vector<string>& sourceFiles, size_t& sizeSourceFiles)
{
    PathName sourceDir(path / subDir);
    CollectFiles(sourceDir, PathName(), runFiles, sizeRunFiles, docFiles, sizeDocFiles, sourceFiles, sizeSourceFiles);
}

void PackageCreator::CollectPackage(MpcPackageInfo& packageInfo)
{
    // clear file lists
    packageInfo.sizeRunFiles = 0;
    packageInfo.runFiles.clear();
    packageInfo.sizeDocFiles = 0;
    packageInfo.docFiles.clear();
    packageInfo.sizeSourceFiles = 0;
    packageInfo.sourceFiles.clear();

    // collect files
    CollectSubTree(packageInfo.path, "Files", packageInfo.runFiles, packageInfo.sizeRunFiles, packageInfo.docFiles, packageInfo.sizeDocFiles, packageInfo.sourceFiles, packageInfo.sizeSourceFiles);
}

void PackageCreator::CollectPackages(const PathName& stagingRoot, map<string, MpcPackageInfo>& packageTable)
{
    if (!Directory::Exists(stagingRoot))
    {
        return;
    }

    unique_ptr<DirectoryLister> lister = DirectoryLister::Open(stagingRoot);

    DirectoryEntry dirEntry;

    while (lister->GetNext(dirEntry))
    {
        if (!dirEntry.isDirectory)
        {
            continue;
        }

        // path to staging directory
        PathName stagingDir(stagingRoot / dirEntry.name);

        // check to see if package.ini exists
        if (!File::Exists(stagingDir / "package.ini"))
        {
            continue;
        }

        // read package.ini and Description
        MpcPackageInfo packageInfo = InitializePackageInfo(stagingDir);

        if (IsToBeIgnored(packageInfo))
        {
            continue;
        }

        Verbose(fmt::format(T_("Collecting {0}..."), Q_(packageInfo.id)));

        // ignore duplicates
        map<string, MpcPackageInfo>::const_iterator it = packageTable.find(packageInfo.id);
        if (it != packageTable.end())
        {
            Warning(fmt::format(T_("{0} already collected."), Q_(packageInfo.id)));
            continue;
        }

        // collect package
        CollectPackage(packageInfo);

        // store package
        packageTable[packageInfo.id] = packageInfo;
    }

    lister->Close();
}

void PackageCreator::BuildTDS(const map<string, MpcPackageInfo>& packageTable, const PathName& destDir, Cfg& repositoryManifest)
{
    for (const pair<string, MpcPackageInfo>& p : packageTable)
    {
        if (IsToBeIgnored(p.second))
        {
            continue;
        }

        // assemble package
        CopyPackage(p.second, destDir);

        // update manifest
        string level;
        level = GetPackageLevel(p.second);
        repositoryManifest.PutValue(p.second.id, "Level", level);
        repositoryManifest.PutValue(p.second.id, "MD5", p.second.digest.ToString());
        repositoryManifest.PutValue(p.second.id, "TimePackaged", std::to_string(programStartTime));
        if (!p.second.version.empty())
        {
            repositoryManifest.PutValue(p.second.id, "Version", p.second.version);
        }
        if (!p.second.targetSystem.empty())
        {
            repositoryManifest.PutValue(p.second.id, "TargetSystem", p.second.targetSystem);
        }
        if (!p.second.minTargetSystemVersion.empty())
        {
            repositoryManifest.PutValue(p.second.id, "MinTargetSystemVersion", p.second.minTargetSystemVersion);
        }
    }
}

void PackageCreator::WritePackageManifestFiles(const map<string, MpcPackageInfo>& packageTable, const PathName& destDir, Cfg& repositoryManifest)
{
    // create package manifest directory
    Directory::Create(destDir);

    Verbose(fmt::format(T_("writing package manifest files in {0}..."), Q_(destDir)));

    for (const pair<string, MpcPackageInfo>& p : packageTable)
    {
        if (IsToBeIgnored(p.second))
        {
            continue;
        }

        // path to package manifest file
        PathName packageManifestFile(destDir / p.second.id);
        packageManifestFile.AppendExtension(MIKTEX_PACKAGE_MANIFEST_FILE_SUFFIX);

        // remove existing package manifest file
        if (File::Exists(packageManifestFile))
        {
            File::Delete(packageManifestFile);
        }

        // write the package manifest file
        string str;
        time_t timePackaged;
        if (repositoryManifest.TryGetValueAsString(p.second.id, "TimePackaged", str))
        {
            timePackaged = Utils::ToTimeT(str);
        }
        else
        {
            timePackaged = InvalidTimeT;
        }
        PackageManager::WritePackageManifestFile(packageManifestFile, p.second, timePackaged);
    }
}

void PackageCreator::DumpPackageManifests(const map<string, MpcPackageInfo>& packageTable, const PathName& path, Cfg& repositoryManifest)
{
    Verbose(fmt::format(T_("dumping package manifests to {0}..."), Q_(path)));
    unique_ptr<Cfg> cfg = Cfg::Create();
    for (const pair<string, MpcPackageInfo>& p : packageTable)
    {
        if (IsToBeIgnored(p.second))
        {
            continue;
        }
        string str;
        time_t timePackaged = InvalidTimeT;
        if (repositoryManifest.TryGetValueAsString(p.second.id, "TimePackaged", str))
        {
            timePackaged = Utils::ToTimeT(str);
        }
        PackageManager::PutPackageManifest(*cfg, p.second, timePackaged);
    }
    if (privateKeyProvider.GetPrivateKeyFile().Empty())
    {
        cfg->Write(path);
    }
    else
    {
        cfg->Write(path, "", &privateKeyProvider);
    }
}

bool PackageCreator::OnProcessOutput(const void* output, size_t n)
{
    processOutput.Append(reinterpret_cast<const char*>(output), n);
    return true;
}

void PackageCreator::ExecuteSystemCommand(const string& command, const PathName& workingDirectory)
{
    processOutput.Clear();
    int exitCode = 0;
    if (workingDirectory.Empty())
    {
        Verbose(fmt::format(T_("working directory: {0}"), PathName().SetToCurrentDirectory().ToDisplayString()));
    }
    else
    {
        Verbose(fmt::format(T_("working directory: {0}"), workingDirectory.ToDisplayString()));
    }
    Verbose(fmt::format(T_("running: {0}"), command));
    if (!Process::ExecuteSystemCommand(command, &exitCode, this, workingDirectory.Empty() ? nullptr : workingDirectory.GetData()) || exitCode != 0)
    {
        cerr << command << ':' << endl;
        cerr << processOutput.GetData() << endl;
        FatalError(T_("A system command failed."));
    }
}

void PackageCreator::RunArchiver(ArchiveFileType archiveFileType, const PathName& archiveFile, const string& filter)
{
    string command;
    switch (archiveFileType)
    {
    case ArchiveFileType::TarBzip2:
        command = "tar -cjf ";
        command += Q_(archiveFile);
        command += " ";
        command += filter;
        break;
    case ArchiveFileType::TarLzma:
        command = fmt::format("tar -cf - {0} | {1} --compress --format=lzma > {2}", filter, Q_(xzExe), Q_(archiveFile));
        break;
    default:
        FatalError(T_("Unsupported archive file type."));
    }
    if (File::Exists(archiveFile))
    {
        File::Delete(archiveFile);
    }
    ExecuteSystemCommand(command.c_str());
}

void PackageCreator::CreateRepositoryInformationFile(const PathName& repository, Cfg& repositoryManifest, const map<string, MpcPackageInfo>& packageTable)
{
    int numberOfPackages = repositoryManifest.GetSize();
    set<MpcPackageInfo, PackagedOnReversed> packagedOnReversed;
    for (const pair<string, MpcPackageInfo>& p : packageTable)
    {
        MpcPackageInfo pi = p.second;
        string str;
        if (repositoryManifest.TryGetValueAsString(p.second.id, "TimePackaged", str))
        {
            pi.timePackaged = Utils::ToTimeT(str);
        }
        else
        {
            pi.timePackaged = InvalidTimeT;
        }
        packagedOnReversed.insert(pi);
    }
    int count = 0;
    string lastupd;
    for (const MpcPackageInfo& pi : packagedOnReversed)
    {
        if (count > 0)
        {
            lastupd += " ";
        }
        lastupd += pi.id;
        if (++count == 20)
        {
            break;
        }
    }
    const time_t t2000 = 946681200;
    int days = static_cast<int>((programStartTime - t2000) / (60 * 60 * 24));
    unique_ptr<Cfg> cfg(Cfg::Create());
    cfg->PutValue("repository", "date", std::to_string(programStartTime));
    cfg->PutValue("repository", "version", std::to_string(days));
    cfg->PutValue("repository", "lstdigest", MD5Builder().Final().ToString());
    cfg->PutValue("repository", "numpkg", std::to_string(numberOfPackages));
    cfg->PutValue("repository", "lastupd", lastupd);
    cfg->PutValue("repository", "relstate", releaseState);
    PathName path(repository);
    path /= "pr.ini";
    if (File::Exists(path))
    {
        File::Delete(path);
    }
    if (privateKeyProvider.GetPrivateKeyFile().Empty())
    {
        cfg->Write(path);
    }
    else
    {
        cfg->Write(path, "", &privateKeyProvider);
    }
    MD5Builder lstDigest;
    unique_ptr<DirectoryLister> lister = DirectoryLister::Open(repository);
    DirectoryEntry2 dirEntry;
    vector<string> lst;
    while (lister->GetNext(dirEntry))
    {
        string line = dirEntry.name;
        line += ';';
        line += std::to_string(static_cast<int>(dirEntry.size));
        line += '\n';
        lst.push_back(line);
    }
    lister->Close();
    lister = nullptr;
    sort(lst.begin(), lst.end());
    for (const string& s : lst)
    {
        lstDigest.Update(s.c_str(), s.length());
    }
    cfg->PutValue("repository", "lstdigest", lstDigest.Final().ToString());
    if (privateKeyProvider.GetPrivateKeyFile().Empty())
    {
        cfg->Write(path);
    }
    else
    {
        cfg->Write(path, "", &privateKeyProvider);
    }
}

void PackageCreator::CreateFileListFile(const map<string, MpcPackageInfo>& packageTable, const PathName& repository)
{
    vector<string> lines;
    size_t prefixLength = texmfPrefix.length();
    if (!PathName(texmfPrefix).EndsWithDirectoryDelimiter())
    {
        prefixLength += 1;
    }
    for (const pair<string, MpcPackageInfo>& p : packageTable)
    {
        if (IsToBeIgnored(p.second))
        {
            continue;
        }
        for (const string& fileName : p.second.docFiles)
        {
            string line = fileName.substr(prefixLength);
            line += ';';
            line += p.second.id;
            lines.push_back(line);
        }
        for (const string& fileName : p.second.runFiles)
        {
            string line = fileName.substr(prefixLength);
            line += ';';
            line += p.second.id;
            lines.push_back(line);
        }
        for (const string& fileName : p.second.sourceFiles)
        {
            string line = fileName.substr(prefixLength);
            line += ';';
            line += p.second.id;
            lines.push_back(line);
        }
    }
    sort(lines.begin(), lines.end());
    PathName filesCsv(repository);
    filesCsv /= "files.csv";
    PathName filesCsvLzma(filesCsv);
    filesCsvLzma.AppendExtension(".lzma");
    ofstream writer = File::CreateOutputStream(filesCsv);
    for (const string& line : lines)
    {
        writer << line << "\n";
    }
    writer.close();
    string command = fmt::format("{0} --compress --format=lzma --keep --stdout {1} > {2}", Q_(xzExe), Q_(filesCsv), Q_(filesCsvLzma));
    if (File::Exists(filesCsvLzma))
    {
        File::Delete(filesCsvLzma);
    }
    ExecuteSystemCommand(command.c_str());
    File::Delete(filesCsv);
}

void PackageCreator::CleanUp(const PathName& repository)
{
    vector<string> toBeDeleted;
    unique_ptr<DirectoryLister> lister = DirectoryLister::Open(repository);
    DirectoryEntry2 dirEntry;
    while (lister->GetNext(dirEntry))
    {
        PathName path(repository);
        path /= dirEntry.name;
        PathName path2;
        if (path.HasExtension(".cab"))
        {
            path2 = path;
            path2.AppendExtension(".tar.bz2");
            if (File::Exists(path2))
            {
                toBeDeleted.push_back(path.ToString());
            }
            path2 = path;
            path2.AppendExtension(".tar.lzma");
            if (File::Exists(path2))
            {
                toBeDeleted.push_back(path.ToString());
            }
        }
        else if (path.HasExtension(".bz2"))
        {
            path2 = path;
            path2.AppendExtension(".lzma");
            if (File::Exists(path2))
            {
                toBeDeleted.push_back(path.ToString());
            }
        }
    }
    for (const string& fileName : toBeDeleted)
    {
        Verbose(fmt::format("Removing {0}...", Q_(fileName)));
        File::Delete(PathName(fileName));
    }
}

void PackageCreator::WriteDatabase(const map<string, MpcPackageInfo>& packageTable, const PathName& repository, bool removeObsoleteSections, Cfg& repositoryManifest)
{
    // create repository
    Directory::Create(repository);

    // change into repository
    Directory::SetCurrent(repository);

    if (removeObsoleteSections)
    {
        // find obsolete package sections
        vector<string> obsoletePackages;
        for (const shared_ptr<Cfg::Key>& key : repositoryManifest)
        {
            map<string, MpcPackageInfo>::const_iterator it = packageTable.find(key->GetName());
            if (it == packageTable.end() || IsToBeIgnored(it->second))
            {
                obsoletePackages.push_back(key->GetName());
            }
        }

        // remove obsolete package sections
        for (const string& fileName : obsoletePackages)
        {
            repositoryManifest.DeleteKey(fileName);
        }
    }

    // create temporary mpm.ini
    unique_ptr<TemporaryFile> tempIni = TemporaryFile::Create(repository / MIKTEX_MPM_INI_FILENAME);
    if (privateKeyProvider.GetPrivateKeyFile().Empty())
    {
        repositoryManifest.Write(tempIni->GetPathName());
    }
    else
    {
        repositoryManifest.Write(tempIni->GetPathName(), "", &privateKeyProvider);
    }

    // create repository manifest archive
    PathName dbPath1(GetRepositoryManifestArchiveFileName());
    RunArchiver(GetDbArchiveFileType(), dbPath1, MIKTEX_MPM_INI_FILENAME);

    // delete temporary mpm.ini
    tempIni = nullptr;

    // create temporary package manifest directory
    unique_ptr<TemporaryDirectory> tempDir = TemporaryDirectory::Create(repository / texmfPrefix);
    PathName packageManifestDir = tempDir->GetPathName();
    packageManifestDir /= MIKTEX_PATH_PACKAGE_MANIFEST_DIR;
    Directory::Create(packageManifestDir);

    // write all package manifest files
    WritePackageManifestFiles(packageTable, packageManifestDir, repositoryManifest);

    // create TPM archive
    PathName dbPath2(GetTpmArchiveFileName());
    RunArchiver(GetDbArchiveFileType(), dbPath2, texmfPrefix.c_str());

    // delete package manifest files
    tempDir = nullptr;

    // create temporary package-manifests.ini
    tempIni = TemporaryFile::Create(repository / "package-manifests.ini");
    DumpPackageManifests(packageTable, tempIni->GetPathName(), repositoryManifest);

    // create package-manifests.ini archive
    PathName dbPath3(GetPackageManifestsArchiveFileName());
    RunArchiver(GetDbArchiveFileType(), dbPath3, "package-manifests.ini");

    // delete temporary package-manifests.ini
    tempIni = nullptr;

    CreateFileListFile(packageTable, repository);

    CleanUp(repository);

    // create pr.ini
    CreateRepositoryInformationFile(repository, repositoryManifest, packageTable);
}

void PackageCreator::Extract(const PathName& archiveFile, ArchiveFileType archiveFileType, const PathName& outDir)
{
    string command;
    switch (archiveFileType)
    {
    case ArchiveFileType::MSCab:
        command = "cabextract ";
        command += Q_(archiveFile);
        break;
    case ArchiveFileType::TarBzip2:
        command = "tar --force-local -xjf ";
        command += Q_(archiveFile);
        break;
    case ArchiveFileType::TarLzma:
        command = fmt::format("{0} --decompress --format=lzma --keep --stdout {1} | tar --force-local -xf -", Q_(xzExe), Q_(archiveFile));
        break;
    default:
        FatalError(T_("Unsupported archive file type."));
    }
    ExecuteSystemCommand(command.c_str(), outDir);
}

void PackageCreator::ExtractFile(const PathName& archiveFile, ArchiveFileType archiveFileType, const PathName& toBeExtracted, const PathName& outFile)
{
    string command;
    switch (archiveFileType)
    {
    case ArchiveFileType::MSCab:
        command = "cabextract --filter ";
        command += Q_(toBeExtracted);
        command += " --pipe ";
        command += Q_(archiveFile);
        command += " > ";
        command += Q_(outFile);
        break;
    case ArchiveFileType::TarBzip2:
        command = "tar --force-local --to-stdout -xjf ";
        command += Q_(archiveFile);
        command += " ";
        command += Q_(toBeExtracted);
        command += " > ";
        command += Q_(outFile);
        break;
    case ArchiveFileType::TarLzma:
        command = fmt::format("{0} --decompress --format=lzma --keep --stdout {1} | tar --force-local --to-stdout -xf - {2} > {3}",
            Q_(xzExe), Q_(archiveFile), Q_(toBeExtracted), Q_(outFile));
        break;
    default:
        FatalError(T_("Unsupported archive file type."));
    }
    ExecuteSystemCommand(command.c_str());
}

void PackageCreator::CompressArchive(const PathName& toBeCompressed, ArchiveFileType archiveFileType, const PathName& outFile)
{
    string command;
    switch (archiveFileType)
    {
    case ArchiveFileType::TarBzip2:
        command = "bzip2 --keep --compress --stdout ";
        command += Q_(toBeCompressed);
        command += " > ";
        command += Q_(outFile);
        break;
    case ArchiveFileType::TarLzma:
        command = fmt::format("{0} --compress --format=lzma --keep --stdout {1} > {2}", Q_(xzExe), Q_(toBeCompressed), Q_(outFile));
        break;
    default:
        FatalError(T_("Unsupported archive file type."));
    }
    ExecuteSystemCommand(command.c_str());
    File::Delete(toBeCompressed);
}

bool PackageCreator::HavePackageArchiveFile(const PathName& repository, const string& packageId, PathName& archiveFile, ArchiveFileType& archiveFileType)
{
    PathName archiveFile2;

    archiveFileType = ArchiveFileType::None;

    // check to see whether a cabinet file exists
    archiveFile2 = repository / packageId;
    archiveFile2.AppendExtension(MIKTEX_CABINET_FILE_SUFFIX);
    if (File::Exists(archiveFile2))
    {
        archiveFile = archiveFile2;
        archiveFileType = ArchiveFileType::MSCab;
    }

    // check to see whether a .tar.bz2 file exists
    archiveFile2 = repository / packageId;
    archiveFile2.AppendExtension(MIKTEX_TARBZIP2_FILE_SUFFIX);
    if (File::Exists(archiveFile2))
    {
        archiveFile = archiveFile2;
        archiveFileType = ArchiveFileType::TarBzip2;
    }

    // check to see whether a .tar.lzma file exists
    archiveFile2 = repository / packageId;
    archiveFile2.AppendExtension(MIKTEX_TARLZMA_FILE_SUFFIX);
    if (File::Exists(archiveFile2))
    {
        archiveFile = archiveFile2;
        archiveFileType = ArchiveFileType::TarLzma;
    }

    return archiveFileType != ArchiveFileType::None;
}

ArchiveFileType PackageCreator::CreateArchiveFile(MpcPackageInfo& packageInfo, const PathName& repository, Cfg& repositoryManifest)
{
    PathName archiveFile;
    ArchiveFileType archiveFileType(ArchiveFileType::None);

    bool reuseExisting = false;

    if (HavePackageArchiveFile(repository, packageInfo.id, archiveFile, archiveFileType))
    {
#if 0
        Verbose(fmt::format(T_("Checking {0}..."), Q_(archiveFile)));
#endif

        // don't remake archive file if there are no changes
        string strMD5;
        string strTimePackaged;
        if (repositoryManifest.TryGetValueAsString(packageInfo.id, "MD5", strMD5)
            && MD5::Parse(strMD5.c_str()) == packageInfo.digest
            && repositoryManifest.TryGetValueAsString(packageInfo.id, "TimePackaged", strTimePackaged))
        {
            packageInfo.timePackaged = Utils::ToTimeT(strTimePackaged);
            reuseExisting = true;
        }
#if 1
        else
        {
            // extract the package manifest file
            PathName filter(texmfPrefix);
            filter /= MIKTEX_PATH_PACKAGE_MANIFEST_DIR;
            filter /= packageInfo.id;
            filter.AppendExtension(MIKTEX_PACKAGE_MANIFEST_FILE_SUFFIX);
#if defined(MIKTEX_WINDOWS)
            filter.ConvertToUnix();
#endif
            unique_ptr<TemporaryFile> packageManifestFile = TemporaryFile::Create();
            ExtractFile(archiveFile, archiveFileType, filter, packageManifestFile->GetPathName());

            // read the package manifest file
            PackageInfo existingPackageInfo = PackageManager::ReadPackageManifestFile(packageManifestFile->GetPathName(), texmfPrefix);

            // check to see whether we can keep the existing file
            if (packageInfo.digest == existingPackageInfo.digest)
            {
                reuseExisting = true;
                packageInfo.timePackaged = existingPackageInfo.timePackaged;
            }
            else
            {
                archiveFileType = ArchiveFileType::None;
            }
        }
#endif
    }

    if (!reuseExisting)
    {
#if 1
        archiveFileType = defaultArchiveFileType;
#else
        archiveFileType = GetPackageArchiveFileType(packageInfo);
#endif

        PathName packageArchiveFile(packageInfo.id);
        packageArchiveFile.AppendExtension
        (PackageCreator::GetFileNameExtension(archiveFileType));

        Verbose(fmt::format(T_("Creating {0}..."), Q_(packageArchiveFile)));

        // create destination directory
        Directory::Create(repository);

        // change into package directory, e.g.:
        // /mypackages/a0poster/
        Directory::SetCurrent(packageInfo.path);

        // path to package manifest directory, e.g.:
        // /mypackages/a0poster/Files/texmf/tpm/packages/
        PathName packageManifestDir(packageInfo.path);
        packageManifestDir /= "Files";
        packageManifestDir /= texmfPrefix;
        packageManifestDir /= MIKTEX_PATH_PACKAGE_MANIFEST_DIR;

        // create package manifest directory
        Directory::Create(packageManifestDir);

        // path to package manifest file, e.g.:
        // /mypackages/a0poster/Files/texmf/tpm/packages/a0poster.tpm
        PathName packageManifestFile(packageManifestDir / packageInfo.id);
        packageManifestFile.AppendExtension(MIKTEX_PACKAGE_MANIFEST_FILE_SUFFIX);

#if 1
        // keep the time-stamp, if possible
        string strMD5;
        string strTimePackaged;
        if (repositoryManifest.TryGetValueAsString(packageInfo.id, "MD5", strMD5)
            && MD5::Parse(strMD5.c_str()) == packageInfo.digest
            && repositoryManifest.TryGetValueAsString(packageInfo.id, "TimePackaged", strTimePackaged))
        {
            packageInfo.timePackaged = Utils::ToTimeT(strTimePackaged);
        }
        else
        {
            packageInfo.timePackaged = programStartTime;
        }
#endif

        // create the package manifest file
        PackageManager::WritePackageManifestFile(packageManifestFile, packageInfo, packageInfo.timePackaged);

        string command;

        // path to .tar file
        PathName tarFile(repository / packageInfo.id);
        tarFile.AppendExtension(MIKTEX_TAR_FILE_SUFFIX);

        // path to compressed .tar file
        archiveFile = repository / packageInfo.id;
        archiveFile.AppendExtension(PackageCreator::GetFileNameExtension(archiveFileType));

#if defined(MIKTEX_WINDOWS)
        tarFile.ConvertToUnix();
        archiveFile.ConvertToUnix();
#endif

        // create the .tar file
        command = "tar --force-local -cf ";
        command += tarFile.ToString();
#if defined(MIKTEX_WINDOWS) && 0
        command += " --files-from=nul";
#else
        command += " --files-from=/dev/null";
#endif
        ExecuteSystemCommand(command.c_str());
        if (Directory::Exists(PathName("Files")))
        {
            RestoreCurrentDirectory restoreCurrentDirectory(PathName("Files"));
            command = "tar --force-local -rf ";
            command += tarFile.ToString();
            command += " ";
            command += texmfPrefix;
            ExecuteSystemCommand(command.c_str());
        }

        // compress the tar file
        {
            RestoreCurrentDirectory restoreCurrentDirectory(repository);
            if (File::Exists(archiveFile))
            {
                File::Delete(archiveFile);
            }
            CompressArchive(tarFile, archiveFileType, archiveFile);
        }
    }

    // get size of archive file
    packageInfo.archiveFileSize = File::GetSize(archiveFile);

    // get MD5 of archive file
    packageInfo.archiveFileDigest = MD5::FromFile(archiveFile);

    // touch the new archive file
    File::SetTimes(archiveFile, reuseExisting ? static_cast<time_t>(-1) : programStartTime, static_cast<time_t>(-1), packageInfo.timePackaged);

    return archiveFileType;
}

unique_ptr<Cfg> PackageCreator::LoadRepositoryManifest(const PathName& repository)
{
    // path to the repository manifest archive file
    PathName pathRepositoryManifestArchive = repository;
    pathRepositoryManifestArchive /= GetRepositoryManifestArchiveFileName();
#if defined(MIKTEX_WINDOWS)
    pathRepositoryManifestArchive.ConvertToUnix();
#endif

    // check to see if the archive file exists
    if (!File::Exists(pathRepositoryManifestArchive))
    {
        FatalError("The repository manifest archive file does not exist.");
    }

    // create a temporary file
    unique_ptr<TemporaryFile> tempFile = TemporaryFile::Create();

    // extract mpm.ini:
    ExtractFile(pathRepositoryManifestArchive, GetDbArchiveFileType(), PathName(MIKTEX_MPM_INI_FILENAME), tempFile->GetPathName());

    // parse mpm.ini
    unique_ptr<Cfg> repositoryManifest(Cfg::Create());
    repositoryManifest->Read(tempFile->GetPathName());

    return repositoryManifest;
}

map<string, MpcPackageInfo> PackageCreator::LoadPackageManifests(const PathName& repository)
{
    map<string, MpcPackageInfo> packageTable;

    // path to the TPM archive file
    PathName pathTpmArchive = repository;
    pathTpmArchive /= GetTpmArchiveFileName();
#if defined(MIKTEX_WINDOWS)
    pathTpmArchive.ConvertToUnix();
#endif

    // check to see if the archive file exists
    if (!File::Exists(pathTpmArchive))
    {
        FatalError("The TPM archive file does not exist.");
    }

    // create a temporary directory
    unique_ptr<TemporaryDirectory> tempDir = TemporaryDirectory::Create();

    // extract all package manifest files
    Extract(pathTpmArchive, GetDbArchiveFileType(), tempDir->GetPathName());

    // parse all package manifest files
    PathName directory = tempDir->GetPathName() / texmfPrefix / MIKTEX_PATH_PACKAGE_MANIFEST_DIR;
    unique_ptr<DirectoryLister> pLister = DirectoryLister::Open(directory, "*" MIKTEX_PACKAGE_MANIFEST_FILE_SUFFIX);
    DirectoryEntry direntry;
    while (pLister->GetNext(direntry))
    {
        PathName packageManifestFile(directory);
        packageManifestFile /= direntry.name;
        PackageInfo packageInfo = PackageManager::ReadPackageManifestFile(packageManifestFile, texmfPrefix);
        packageInfo.id = packageManifestFile.GetFileNameWithoutExtension().ToString();
        packageTable[packageInfo.id] = packageInfo;
    }

    return packageTable;
}

void PackageCreator::UpdateRepository(map<string, MpcPackageInfo>& packageTable, const PathName& repository, Cfg& repositoryManifest)
{
    for (pair<const string, MpcPackageInfo>& p : packageTable)
    {
        if (IsToBeIgnored(p.second) || IsPureContainerPackage(p.second))
        {
            continue;
        }

        // update level field
        string level;
        level = GetPackageLevel(p.second);
        repositoryManifest.PutValue(p.second.id, "Level", level);

#if 0
        // get TDS digest of already existing package
        string str;
        if (repositoryManifest.TryGetValueAsString(it->second.packageId, "MD5", str))
        {
            // don't remake archive file if there are no changes
            PathName archiveFile;
            ArchiveFileType archiveFileType(ArchiveFileType::None);
            if (MD5::Parse(str.c_str()) == it->second.digest
                && HavePackageArchiveFile(repository, it->second.packageId, archiveFile, archiveFileType))
            {
#if 0
                Verbose(fmt::format(T_("{0} hasn't changed => skipping"), Q_(it->second.packageId)));
#endif
                continue;
            }
        }
#endif

        // create the archive file
        ArchiveFileType archiveFileType = CreateArchiveFile(p.second, repository, repositoryManifest);

        // update repository manifest
        repositoryManifest.PutValue(p.second.id, "MD5", p.second.digest.ToString());
        repositoryManifest.PutValue(p.second.id, "TimePackaged", std::to_string(p.second.timePackaged));
        repositoryManifest.PutValue(p.second.id, "CabSize", std::to_string(static_cast<int>(p.second.archiveFileSize)));
        repositoryManifest.PutValue(p.second.id, "CabMD5", p.second.archiveFileDigest.ToString());
        repositoryManifest.PutValue(p.second.id, "Type",
            (archiveFileType == ArchiveFileType::MSCab ? "MSCab"
                : (archiveFileType == ArchiveFileType::TarBzip2 ? "TarBzip2"
                    : (archiveFileType == ArchiveFileType::TarLzma ? "TarLzma"
                        : "unknown"))));

        if (p.second.version.empty())
        {
            string oldVersion;
            if (repositoryManifest.TryGetValueAsString(p.second.id, "Version", oldVersion))
            {
                repositoryManifest.DeleteValue(p.second.id, "Version");
            }
        }
        else
        {
            repositoryManifest.PutValue(p.second.id, "Version", p.second.version);
        }
        if (p.second.targetSystem.empty())
        {
            string oldTargetSystem;
            if (repositoryManifest.TryGetValueAsString(p.second.id, "TargetSystem", oldTargetSystem))
            {
                repositoryManifest.DeleteValue(p.second.id, "TargetSystem");
            }
        }
        else
        {
            repositoryManifest.PutValue(p.second.id, "TargetSystem", p.second.targetSystem);
        }
        if (p.second.minTargetSystemVersion.empty())
        {
            string oldMinTargetSystemVersion;
            if (repositoryManifest.TryGetValueAsString(p.second.id, "MinTargetSystemVersion", oldMinTargetSystemVersion))
            {
                repositoryManifest.DeleteValue(p.second.id, "MinTargetSystemVersion");
            }
        }
        else
        {
            repositoryManifest.PutValue(p.second.id, "MinTargetSystemVersion", p.second.minTargetSystemVersion);
        }
    }
}

void PackageCreator::ReadList(const PathName& path, map<string, PackageSpec>& mapPackageList)
{
    ifstream reader = File::CreateInputStream(path);
    string line;
    while (std::getline(reader, line))
    {
        if (line.empty())
        {
            continue;
        }
        char ch = line[0];
        const char* lpsz = line.c_str() + 1;
        while (*lpsz != 0 && (*lpsz == ' ' || *lpsz == '\t'))
        {
            ++lpsz;
        }
        if (*lpsz == 0)
        {
            continue;
        }
        if (ch == '@')
        {
            ReadList(PathName(lpsz), mapPackageList);
            continue;
        }
        if (ch != 'S' && ch != 'M' && ch != 'L' && ch != 'T' && ch != '-')
        {
            continue;
        }
        Tokenizer tok(lpsz, ";");
        string packageId = *tok;
        auto it = mapPackageList.find(packageId);
        if (it != mapPackageList.end())
        {
            Warning(fmt::format(T_("ignoring '{0} {1}': already marked as '{2}'"), ch, Q_(packageId), it->second.level));
            continue;
        }
        PackageSpec pkgspec;
        pkgspec.id = packageId;
        pkgspec.level = ch;
        ArchiveFileType archiveFileType = defaultArchiveFileType;
        ++tok;
        if (tok)
        {
            if (*tok == "MSCab")
            {
                archiveFileType = ArchiveFileType::MSCab;
            }
            else if (*tok == "TarBzip2")
            {
                archiveFileType = ArchiveFileType::TarBzip2;
            }
            else if (*tok == "TarLzma")
            {
                archiveFileType = ArchiveFileType::TarLzma;
            }
            else
            {
                FatalError("Invalid package list file.");
            }
        }
        pkgspec.archiveFileType = archiveFileType;
        mapPackageList[pkgspec.id] = pkgspec;
    }
    reader.close();
}

void PackageCreator::ReadList(const PathName& path, set<string>& packageList)
{
    ifstream stream = File::CreateInputStream(path);
    string line;
    while (std::getline(stream, line))
    {
        size_t l = line.length();
        if (l == 0)
        {
            continue;
        }
        if (line[l - 1] == '\n')
        {
            --l;
            line.erase(l);
        }
        if (l == 0)
        {
            continue;
        }
        packageList.insert(line);
    }
    stream.close();
}

void PackageCreator::DisassemblePackage(const PathName& packageManifestFile, const PathName& sourceDir, const PathName& stagingDir)
{
    // parse the package manifest file
    Verbose(fmt::format(T_("Parsing {0}..."), Q_(packageManifestFile)));
    PackageInfo packageInfo = PackageManager::ReadPackageManifestFile(packageManifestFile, texmfPrefix);

    // remove the package manifest file from the RunFiles list
    const char* lpszRelPath = Utils::GetRelativizedPath(packageManifestFile.GetData(), sourceDir.GetData());
    if (lpszRelPath != nullptr)
    {
        vector<string>::iterator it;
        for (it = packageInfo.runFiles.begin(); it != packageInfo.runFiles.end(); ++it)
        {
            if (PathName::Equals(PathName(*it), PathName(lpszRelPath)))
            {
                break;
            }
        }
        if (it != packageInfo.runFiles.end())
        {
            packageInfo.runFiles.erase(it);
        }
    }

    // determine the package ID, e.g.:
    // a0poster
    packageInfo.id = packageManifestFile.GetFileNameWithoutExtension().ToString();

    Verbose(fmt::format(" {0} ({1} files)...", packageInfo.id, packageInfo.GetNumFiles()));

    // copy files and calculate checksums; the package manifest file
    // has been removed from the RunFiles list
    FileDigestTable fileDigests;
    MD5CopyFiles(packageInfo.runFiles, sourceDir, nullptr, stagingDir, "Files", fileDigests);
    MD5CopyFiles(packageInfo.docFiles, sourceDir, nullptr, stagingDir, "Files", fileDigests);
    MD5CopyFiles(packageInfo.sourceFiles, sourceDir, nullptr, stagingDir, "Files", fileDigests);

    // calculate TDS digest
    MD5 tdsDigest = GetTdsDigest(fileDigests);

    // write package.ini, md5sums.txt and Description, e.g.:
    // /mypackages/a0poster/package.ini
    // /mypackages/a0poster/md5sums.txt
    // /mypackages/a0poster/Description
    InitializeStagingDirectory(stagingDir, packageInfo, fileDigests, tdsDigest);

    // write new package manifest file, e.g.:
    // /mypackages/a0poster/Files/texmf/tpm/packages/a0poster.tpm
    MpcPackageInfo mpcPackageInfo(packageInfo);
    mpcPackageInfo.digest = tdsDigest;
    mpcPackageInfo.path = stagingDir;
    CollectPackage(mpcPackageInfo);
    PathName packageManifestDir(stagingDir);
    packageManifestDir /= "Files";
    packageManifestDir /= texmfPrefix;
    packageManifestDir /= MIKTEX_PATH_PACKAGE_MANIFEST_DIR;
    Directory::Create(packageManifestDir);
    PackageManager::WritePackageManifestFile((packageManifestDir / packageInfo.id).AppendExtension(MIKTEX_PACKAGE_MANIFEST_FILE_SUFFIX), mpcPackageInfo, 0);
}

void PackageCreator::Run(int argc, const char** argv)
{
    vector<string> stagingRoots;

    PathName packageManifestFile;
    PathName repository;
    PathName stagingDir;
    PathName texmfParent;
    PathName tpmDir;

    bool optBuildTDS = false;
    bool optCreatePackage = false;
    bool optDisassemblePackage = false;
    bool optUpdateRepository = false;
    bool optVersion = false;

    optVerbose = false;
    programStartTime = time(nullptr);

    PoptWrapper popt(argc, argv, options);

    int option;

    while ((option = popt.GetNextOpt()) >= 0)
    {
        string optArg = popt.GetOptArg();
        switch (option)
        {
        case OPT_BUILD_TDS:
            optBuildTDS = true;
            break;
        case OPT_CREATE_PACKAGE:
            optCreatePackage = true;
            break;
        case OPT_DEFAULT_LEVEL:
            if (optArg.empty())
            {
                FatalError(T_("Missing package level."));
            }
            defaultLevel = optArg[0];
            break;
        case OPT_DISASSEMBLE_PACKAGE:
            optDisassemblePackage = true;
            break;
        case OPT_MIKTEX_MAJOR_MINOR:
            majorMinorVersion = VersionNumber(optArg);
            if (majorMinorVersion.CompareTo(VersionNumber(MIKTEX_PACKAGES_MAJOR_MINOR_STR)) > 0)
            {
                FatalError(T_("Unsupported MiKTeX major/minor version."));
            }
            break;
        case OPT_PACKAGE_LIST:
            ReadList(PathName(optArg), packageList);
            break;
        case OPT_PASSPHRASE_FILE:
            privateKeyProvider.SetPassphrase(File::ReadAllBytes(PathName(optArg)));
            break;
        case OPT_PRIVATE_KEY_FILE:
            privateKeyProvider.SetPrivateKeyFile(PathName(optArg));
            break;
        case OPT_RELEASE_STATE:
            releaseState = optArg;
            break;
        case OPT_REPOSITORY:
            repository = optArg;
            break;
        case OPT_STAGING_DIR:
            stagingDir = optArg;
            break;
        case OPT_STAGING_ROOTS:
        {
            for (const string& dir : StringUtil::Split(optArg, PathNameUtil::PathNameDelimiter))
            {
                stagingRoots.push_back(dir);
            }
            break;
        }
        case OPT_TEXMF_PARENT:
            texmfParent = optArg;
            break;
        case OPT_TEXMF_PREFIX:
            texmfPrefix = optArg;
            break;
        case OPT_TIME_PACKAGED:
            programStartTime = Utils::ToTimeT(optArg);
            break;
        case OPT_TPM_DIR:
            tpmDir = optArg;
            break;
        case OPT_TPM_FILE:
            packageManifestFile = optArg;
            break;
        case OPT_UPDATE_REPOSITORY:
            optUpdateRepository = true;
            break;
        case OPT_VERBOSE:
            optVerbose = true;
            break;
        case OPT_VERSION:
            optVersion = true;
            break;
        }
    }

    if (option != -1)
    {
        string msg = popt.BadOption(POPT_BADOPTION_NOALIAS);
        msg += ": ";
        msg += popt.Strerror(option);
        FatalError(msg);
    }

    if (optVersion)
    {
        cout
            << Utils::MakeProgramVersionString(TheNameOfTheGame, VersionNumber(MIKTEX_COMP_MAJOR_VERSION, MIKTEX_COMP_MINOR_VERSION, MIKTEX_COMP_PATCH_VERSION, 0)) << endl
            << "Copyright (C) 2006-2024 Christian Schenk" << endl
            << "This is free software; see the source for copying conditions.  There is NO" << endl
            << "warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE." << endl;
    }
    else if (optCreatePackage)
    {
        if (stagingDir.Empty())
        {
            stagingDir.SetToCurrentDirectory();
        }
        if (repository.Empty())
        {
            FatalError(T_("No repository location was specified."));
        }
        Verbose(fmt::format(T_("Loading repository manifest from {0}..."), Q_(repository)));
        unique_ptr<Cfg> repositoryManifest(LoadRepositoryManifest(repository));
        map<string, MpcPackageInfo> packageTable = LoadPackageManifests(repository);
        Verbose(fmt::format(T_("Reading staging directory {0}..."), Q_(stagingDir)));
        MpcPackageInfo packageInfo = InitializePackageInfo(stagingDir);
        CollectPackage(packageInfo);
        packageTable[packageInfo.id] = packageInfo;
        UpdateRepository(packageTable, repository, *repositoryManifest);
        Verbose(fmt::format(T_("Writing database to {0}..."), Q_(repository)));
        WriteDatabase(packageTable, repository, false, *repositoryManifest);
    }
    else if (optDisassemblePackage)
    {
        if (packageManifestFile.Empty())
        {
            FatalError(T_("No package manifest file has been specified."));
        }
        if (texmfParent.Empty())
        {
            FatalError(T_("No TEXMF parent directory has been specified."));
        }
        if (stagingDir.Empty())
        {
            FatalError(T_("No staging directory has been specified."));
        }
        DisassemblePackage(packageManifestFile, texmfParent, stagingDir);
    }
    else if (optUpdateRepository || optBuildTDS)
    {
        if (stagingRoots.empty())
        {
            FatalError(T_("No staging roots were specified."));
        }

        // collect the packages
        map<string, MpcPackageInfo> packageTable;
        for (const string& r : stagingRoots)
        {
            CollectPackages(PathName(r), packageTable);
        }

        if (packageTable.empty())
        {
            FatalError(T_("No staging directories were found."));
        }

        if (optBuildTDS)
        {
            // create a TeX directory structure
            if (texmfParent.Empty())
            {
                FatalError(T_("No TEXMF parent directory has been specified."));
            }
            unique_ptr<Cfg> repositoryManifest(Cfg::Create());
            BuildTDS(packageTable, texmfParent, *repositoryManifest);
            if (!tpmDir.Empty())
            {
                WritePackageManifestFiles(packageTable, tpmDir, *repositoryManifest);
            }
            // write mpm.ini
            PathName iniFile(texmfParent);
            iniFile /= texmfPrefix;
            iniFile /= MIKTEX_PATH_MPM_INI;
            if (privateKeyProvider.GetPrivateKeyFile().Empty())
            {
                repositoryManifest->Write(iniFile);
            }
            else
            {
                repositoryManifest->Write(iniFile, "", &privateKeyProvider);
            }
        }
        else if (optUpdateRepository)
        {
            if (repository.Empty())
            {
                FatalError(T_("No repository location was specified."));
            }
            // load repository manifest archive
            unique_ptr<Cfg> repositoryManifest(LoadRepositoryManifest(repository));
#if 1
            bool autoCategorize = true;
            if (autoCategorize)
            {
                // determine dependencies
                for (auto& pkg : packageTable)
                {
                    for (const string& req : pkg.second.requiredPackages)
                    {
                        auto it3 = packageTable.find(req);
                        if (it3 == packageTable.end())
                        {
                            Warning(fmt::format(T_("dependency problem: {0} is required by {1}"), req, pkg.second.id));
                        }
                        else
                        {
                            it3->second.requiredBy.push_back(pkg.second.id);
                        }
                    }
                }
                // categorize
                auto latex = packageTable.find("_miktex-latex-packages");
                auto outlineFonts = packageTable.find("_miktex-fonts-type1");
                auto isOutlineFont = [](const string& s)
                {
                    return Utils::IsParentDirectoryOf(PathName("texmf/fonts/type1"), PathName(s)) || Utils::IsParentDirectoryOf(PathName("texmf/fonts/truetype"), PathName(s));
                };
                for (auto& pkg : packageTable)
                {
                    const auto& ctanPath = pkg.second.ctanPath;
                    const auto& runFiles = pkg.second.runFiles;
                    if (pkg.second.requiredBy.empty())
                    {
                        if (latex != packageTable.end() && StartsWith(ctanPath, "/macros/latex/contrib/"))
                        {
                            pkg.second.requiredBy.push_back(latex->second.id);
                            latex->second.requiredPackages.push_back(pkg.second.id);
                        }
                        else if (outlineFonts != packageTable.end()
                            && StartsWith(ctanPath, "/fonts/")
                            && std::any_of(runFiles.begin(), runFiles.end(), isOutlineFont))
                        {
                            pkg.second.requiredBy.push_back(outlineFonts->second.id);
                            outlineFonts->second.requiredPackages.push_back(pkg.second.id);
                        }
                    }
                }
            }
#endif
            UpdateRepository(packageTable, repository, *repositoryManifest);
            WriteDatabase(packageTable, repository, true, *repositoryManifest);
        }
    }
    else
    {
        FatalError(T_("No task was specified."));
    }
}

int main(int argc, const char** argv)
{
    int result;

    try
    {
        PackageCreator app;
        app.Init(argv[0]);
        app.Run(argc, argv);
        app.Finalize();
        result = 0;
    }

    catch (const MiKTeXException& e)
    {
        Utils::PrintException(e);
        result = 1;
    }

    catch (const exception& e)
    {
        Utils::PrintException(e);
        result = 1;
    }

    catch (int i)
    {
        result = i;
    }

    return result;
}
