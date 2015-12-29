#include "testing/testing.hpp"

#include "storage/storage.hpp"
#include "storage/storage_defines.hpp"
#include "storage/storage_tests/fake_map_files_downloader.hpp"
#include "storage/storage_tests/task_runner.hpp"
#include "storage/storage_tests/test_map_files_downloader.hpp"

#include "indexer/indexer_tests/test_mwm_set.hpp"

#include "platform/country_file.hpp"
#include "platform/local_country_file.hpp"
#include "platform/local_country_file_utils.hpp"
#include "platform/mwm_version.hpp"
#include "platform/platform.hpp"
#include "platform/platform_tests_support/scoped_dir.hpp"
#include "platform/platform_tests_support/scoped_file.hpp"

#include "coding/file_name_utils.hpp"
#include "coding/file_writer.hpp"
#include "coding/internal/file_data.hpp"

#include "defines.hpp"

#include "base/scope_guard.hpp"
#include "base/string_utils.hpp"

#include "std/bind.hpp"
#include "std/condition_variable.hpp"
#include "std/map.hpp"
#include "std/mutex.hpp"
#include "std/shared_ptr.hpp"
#include "std/unique_ptr.hpp"
#include "std/vector.hpp"

#include <QtCore/QCoreApplication>

using namespace platform;

namespace storage
{
namespace
{

using TLocalFilePtr = shared_ptr<LocalCountryFile>;

// This class checks steps Storage::DownloadMap() performs to download a map.
class CountryDownloaderChecker
{
public:
  CountryDownloaderChecker(Storage & storage, TIndex const & index, MapOptions files,
                           vector<TStatus> const & transitionList)
      : m_storage(storage),
        m_index(index),
        m_countryFile(storage.GetCountryFile(m_index)),
        m_files(files),
        m_bytesDownloaded(0),
        m_totalBytesToDownload(0),
        m_slot(0),
        m_currStatus(0),
        m_transitionList(transitionList)
  {
    m_slot = m_storage.Subscribe(
        bind(&CountryDownloaderChecker::OnCountryStatusChanged, this, _1),
        bind(&CountryDownloaderChecker::OnCountryDownloadingProgress, this, _1, _2));
    TEST(storage.IsIndexInCountryTree(index), (m_countryFile));
    TEST(!m_transitionList.empty(), (m_countryFile));
  }

  virtual ~CountryDownloaderChecker()
  {
    TEST_EQUAL(m_currStatus + 1, m_transitionList.size(), (m_countryFile));
    m_storage.Unsubscribe(m_slot);
  }

  void StartDownload()
  {
    TEST_EQUAL(0, m_currStatus, (m_countryFile));
    TEST_LESS(m_currStatus, m_transitionList.size(), (m_countryFile));
    TEST_EQUAL(m_transitionList[m_currStatus], m_storage.CountryStatusEx(m_index), (m_countryFile));
    m_storage.DownloadCountry(m_index, m_files);
  }

protected:
  virtual void OnCountryStatusChanged(TIndex const & index)
  {
    if (index != m_index)
      return;

    TStatus const nextStatus = m_storage.CountryStatusEx(m_index);
    LOG(LINFO, (m_countryFile, "status transition: from", m_transitionList[m_currStatus], "to",
                nextStatus));
    TEST_LESS(m_currStatus + 1, m_transitionList.size(), (m_countryFile));
    TEST_EQUAL(nextStatus, m_transitionList[m_currStatus + 1], (m_countryFile));
    ++m_currStatus;
    if (m_transitionList[m_currStatus] == TStatus::EDownloading)
    {
      LocalAndRemoteSizeT localAndRemoteSize = m_storage.CountrySizeInBytes(m_index, m_files);
      m_totalBytesToDownload = localAndRemoteSize.second;
    }
  }

  virtual void OnCountryDownloadingProgress(TIndex const & index,
                                            LocalAndRemoteSizeT const & progress)
  {
    if (index != m_index)
      return;

    LOG(LINFO, (m_countryFile, "downloading progress:", progress));

    TEST_GREATER(progress.first, m_bytesDownloaded, (m_countryFile));
    m_bytesDownloaded = progress.first;
    TEST_LESS_OR_EQUAL(m_bytesDownloaded, m_totalBytesToDownload, (m_countryFile));

    LocalAndRemoteSizeT localAndRemoteSize = m_storage.CountrySizeInBytes(m_index, m_files);
    TEST_EQUAL(m_totalBytesToDownload, localAndRemoteSize.second, (m_countryFile));
  }

  Storage & m_storage;
  TIndex const m_index;
  CountryFile const m_countryFile;
  MapOptions const m_files;
  int64_t m_bytesDownloaded;
  int64_t m_totalBytesToDownload;
  int m_slot;

  size_t m_currStatus;
  vector<TStatus> m_transitionList;
};

class CancelDownloadingWhenAlmostDoneChecker : public CountryDownloaderChecker
{
public:
  CancelDownloadingWhenAlmostDoneChecker(Storage & storage, TIndex const & index,
                                         TaskRunner & runner)
      : CountryDownloaderChecker(storage, index, MapOptions::Map,
                                 vector<TStatus>{TStatus::ENotDownloaded, TStatus::EDownloading,
                                                 TStatus::ENotDownloaded}),
        m_runner(runner)
  {
  }

protected:
  // CountryDownloaderChecker overrides:
  void OnCountryDownloadingProgress(TIndex const & index,
                                    LocalAndRemoteSizeT const & progress) override
  {
    CountryDownloaderChecker::OnCountryDownloadingProgress(index, progress);

    // Cancel downloading when almost done.
    if (progress.first + 2 * FakeMapFilesDownloader::kBlockSize >= progress.second)
    {
      m_runner.PostTask([&]()
                        {
                          m_storage.DeleteFromDownloader(m_index);
                        });
    }
  }

  TaskRunner & m_runner;
};

// Checks following state transitions:
// NotDownloaded -> Downloading -> OnDisk.
unique_ptr<CountryDownloaderChecker> AbsentCountryDownloaderChecker(Storage & storage,
                                                                    TIndex const & index,
                                                                    MapOptions files)
{
  return make_unique<CountryDownloaderChecker>(
      storage, index, files,
      vector<TStatus>{TStatus::ENotDownloaded, TStatus::EDownloading, TStatus::EOnDisk});
}

// Checks following state transitions:
// OnDisk -> Downloading -> OnDisk.
unique_ptr<CountryDownloaderChecker> PresentCountryDownloaderChecker(Storage & storage,
                                                                     TIndex const & index,
                                                                     MapOptions files)
{
  return make_unique<CountryDownloaderChecker>(
      storage, index, files,
      vector<TStatus>{TStatus::EOnDisk, TStatus::EDownloading, TStatus::EOnDisk});
}

// Checks following state transitions:
// NotDownloaded -> InQueue -> Downloading -> OnDisk.
unique_ptr<CountryDownloaderChecker> QueuedCountryDownloaderChecker(Storage & storage,
                                                                    TIndex const & index,
                                                                    MapOptions files)
{
  return make_unique<CountryDownloaderChecker>(
      storage, index, files, vector<TStatus>{TStatus::ENotDownloaded, TStatus::EInQueue,
                                             TStatus::EDownloading, TStatus::EOnDisk});
}

// Checks following state transitions:
// NotDownloaded -> Downloading -> NotDownloaded.
unique_ptr<CountryDownloaderChecker> CancelledCountryDownloaderChecker(Storage & storage,
                                                                       TIndex const & index,
                                                                       MapOptions files)
{
  return make_unique<CountryDownloaderChecker>(
      storage, index, files,
      vector<TStatus>{TStatus::ENotDownloaded, TStatus::EDownloading, TStatus::ENotDownloaded});
}

class CountryStatusChecker
{
public:
  CountryStatusChecker(Storage & storage, TIndex const & index, TStatus status)
      : m_storage(storage), m_index(index), m_status(status), m_triggered(false)
  {
    m_slot = m_storage.Subscribe(
        bind(&CountryStatusChecker::OnCountryStatusChanged, this, _1),
        bind(&CountryStatusChecker::OnCountryDownloadingProgress, this, _1, _2));
  }

  ~CountryStatusChecker()
  {
    TEST(m_triggered, ("Status checker wasn't triggered."));
    m_storage.Unsubscribe(m_slot);
  }

private:
  void OnCountryStatusChanged(TIndex const & index)
  {
    if (index != m_index)
      return;
    TEST(!m_triggered, ("Status checker can be triggered only once."));
    TStatus status = m_storage.CountryStatusEx(m_index);
    TEST_EQUAL(m_status, status, ());
    m_triggered = true;
  }

  void OnCountryDownloadingProgress(TIndex const & /* index */,
                                    LocalAndRemoteSizeT const & /* progress */)
  {
    TEST(false, ("Unexpected country downloading progress."));
  }

  Storage & m_storage;
  TIndex const & m_index;
  TStatus m_status;
  bool m_triggered;
  int m_slot;
};

class FailedDownloadingWaiter
{
public:
  FailedDownloadingWaiter(Storage & storage, TIndex const & index)
    : m_storage(storage), m_index(index), m_finished(false)
  {
    m_slot = m_storage.Subscribe(bind(&FailedDownloadingWaiter::OnStatusChanged, this, _1),
                                 bind(&FailedDownloadingWaiter::OnProgress, this, _1, _2));
  }

  ~FailedDownloadingWaiter()
  {
    Wait();
    m_storage.Unsubscribe(m_slot);
  }

  void Wait()
  {
    unique_lock<mutex> lock(m_mu);
    m_cv.wait(lock, [this]()
    {
      return m_finished;
    });
  }

  void OnStatusChanged(TIndex const & index)
  {
    if (index != m_index)
      return;
    TStatus const status = m_storage.CountryStatusEx(index);
    if (status != TStatus::EDownloadFailed)
      return;
    lock_guard<mutex> lock(m_mu);
    m_finished = true;
    m_cv.notify_one();

    QCoreApplication::exit();
  }

  void OnProgress(TIndex const & /* index */, LocalAndRemoteSizeT const & /* progress */) {}

private:
  Storage & m_storage;
  TIndex const m_index;
  int m_slot;

  mutex m_mu;
  condition_variable m_cv;
  bool m_finished;
};

void OnCountryDownloaded(LocalCountryFile const & localFile)
{
  LOG(LINFO, ("OnCountryDownloaded:", localFile));
}

TLocalFilePtr CreateDummyMapFile(CountryFile const & countryFile, int64_t version, size_t size)
{
  TLocalFilePtr localFile = PreparePlaceForCountryFiles(countryFile, version);
  TEST(localFile.get(), ("Can't prepare place for", countryFile, "(version", version, ")"));
  {
    string const zeroes(size, '\0');
    FileWriter writer(localFile->GetPath(MapOptions::Map));
    writer.Write(zeroes.data(), zeroes.size());
  }
  localFile->SyncWithDisk();
  TEST_EQUAL(MapOptions::Map, localFile->GetFiles(), ());
  TEST_EQUAL(size, localFile->GetSize(MapOptions::Map), ());
  return localFile;
}

void InitStorage(Storage & storage, TaskRunner & runner,
                 Storage::TUpdate const & update = &OnCountryDownloaded)
{
  storage.Init(update);
  storage.RegisterAllLocalMaps();
  storage.SetDownloaderForTesting(make_unique<FakeMapFilesDownloader>(runner));
}
}  // namespace

UNIT_TEST(StorageTest_Smoke)
{
  Storage storage;

  TIndex const georgiaIndex = storage.FindIndexByFile("Georgia");
  TEST(IsIndexValid(georgiaIndex), ());
  CountryFile usaGeorgiaFile = storage.GetCountryFile(georgiaIndex);
  TEST_EQUAL(platform::GetNameWithTwoComponentsExt(usaGeorgiaFile.GetName(), MapOptions::Map),
             "Georgia" DATA_FILE_EXTENSION, ());

  if (version::IsSingleMwm(storage.GetCurrentDataVersion()))
    return; // Following code tests car routing files, and is not relevant for a single mwm case.

  TEST(IsIndexValid(georgiaIndex), ());
  CountryFile georgiaFile = storage.GetCountryFile(georgiaIndex);
  TEST_EQUAL(platform::GetNameWithTwoComponentsExt(georgiaFile.GetName(), MapOptions::CarRouting),
             "Georgia" DATA_FILE_EXTENSION ROUTING_FILE_EXTENSION, ());
}

UNIT_TEST(StorageTest_SingleCountryDownloading)
{
  Storage storage;
  TaskRunner runner;
  InitStorage(storage, runner);

  string const mwmName = version::IsSingleMwm(storage.GetCurrentDataVersion()) ?
      "Azerbaijan Region" : "Azerbaijan";
  TIndex const azerbaijanIndex = storage.FindIndexByFile(mwmName);
  TEST(IsIndexValid(azerbaijanIndex), ());

  CountryFile azerbaijanFile = storage.GetCountryFile(azerbaijanIndex);
  storage.DeleteCountry(azerbaijanIndex, MapOptions::Map);

  {
    MY_SCOPE_GUARD(cleanupCountryFiles,
                   bind(&Storage::DeleteCountry, &storage, azerbaijanIndex, MapOptions::Map));
    unique_ptr<CountryDownloaderChecker> checker =
        AbsentCountryDownloaderChecker(storage, azerbaijanIndex, MapOptions::Map);
    checker->StartDownload();
    runner.Run();
  }

  {
    MY_SCOPE_GUARD(cleanupCountryFiles, bind(&Storage::DeleteCountry, &storage, azerbaijanIndex,
                                             MapOptions::Map));
    unique_ptr<CountryDownloaderChecker> checker =
        AbsentCountryDownloaderChecker(storage, azerbaijanIndex, MapOptions::Map);
    checker->StartDownload();
    runner.Run();
  }
}

UNIT_TEST(StorageTest_TwoCountriesDownloading)
{
  Storage storage;
  TaskRunner runner;
  InitStorage(storage, runner);

  TIndex const uruguayIndex = storage.FindIndexByFile("Uruguay");
  TEST(IsIndexValid(uruguayIndex), ());
  storage.DeleteCountry(uruguayIndex, MapOptions::Map);
  MY_SCOPE_GUARD(cleanupUruguayFiles,
                 bind(&Storage::DeleteCountry, &storage, uruguayIndex, MapOptions::Map));

  TIndex const venezuelaIndex = storage.FindIndexByFile("Venezuela");
  TEST(IsIndexValid(venezuelaIndex), ());
  storage.DeleteCountry(venezuelaIndex, MapOptions::Map);
  MY_SCOPE_GUARD(cleanupVenezuelaFiles, bind(&Storage::DeleteCountry, &storage, venezuelaIndex,
                                             MapOptions::Map));

  unique_ptr<CountryDownloaderChecker> uruguayChecker =
      AbsentCountryDownloaderChecker(storage, uruguayIndex, MapOptions::Map);
  unique_ptr<CountryDownloaderChecker> venezuelaChecker =
      QueuedCountryDownloaderChecker(storage, venezuelaIndex, MapOptions::Map);
  uruguayChecker->StartDownload();
  venezuelaChecker->StartDownload();
  runner.Run();
}

UNIT_TEST(StorageTest_DeleteTwoVersionsOfTheSameCountry)
{
  Storage storage;
  bool const isSingleMwm = version::IsSingleMwm(storage.GetCurrentDataVersion());
  if (isSingleMwm)
    storage.SetCurrentDataVersionForTesting(version::ForTesting::SingleMwmLatest);
  string const mwmName = isSingleMwm ? "Azerbaijan Region" : "Azerbaijan";
  int64_t const v1 = isSingleMwm ? version::ForTesting::SingleMwm1
                                 : version::ForTesting::TwoComponentMwm1;
  int64_t const v2 = isSingleMwm ? version::ForTesting::SingleMwm2
                                 : version::ForTesting::TwoComponentMwm2;

  storage.Init(&OnCountryDownloaded);
  storage.RegisterAllLocalMaps();

  TIndex const index = storage.FindIndexByFile(mwmName);
  TEST(IsIndexValid(index), ());
  CountryFile const countryFile = storage.GetCountryFile(index);

  storage.DeleteCountry(index, MapOptions::Map);
  TLocalFilePtr latestLocalFile = storage.GetLatestLocalFile(index);
  TEST(!latestLocalFile.get(), ("Country wasn't deleted from disk."));
  TEST_EQUAL(TStatus::ENotDownloaded, storage.CountryStatusEx(index), ());

  TLocalFilePtr localFileV1 = CreateDummyMapFile(countryFile, v1, 1024 /* size */);
  storage.RegisterAllLocalMaps();
  latestLocalFile = storage.GetLatestLocalFile(index);
  TEST(latestLocalFile.get(), ("Created map file wasn't found by storage."));
  TEST_EQUAL(latestLocalFile->GetVersion(), localFileV1->GetVersion(), ());
  TEST_EQUAL(TStatus::EOnDiskOutOfDate, storage.CountryStatusEx(index), ());

  TLocalFilePtr localFileV2 = CreateDummyMapFile(countryFile, v2, 2048 /* size */);
  storage.RegisterAllLocalMaps();
  latestLocalFile = storage.GetLatestLocalFile(index);
  TEST(latestLocalFile.get(), ("Created map file wasn't found by storage."));
  TEST_EQUAL(latestLocalFile->GetVersion(), localFileV2->GetVersion(), ());
  TEST_EQUAL(TStatus::EOnDiskOutOfDate, storage.CountryStatusEx(index), ());

  storage.DeleteCountry(index, MapOptions::Map);

  localFileV1->SyncWithDisk();
  TEST_EQUAL(MapOptions::Nothing, localFileV1->GetFiles(), ());

  localFileV2->SyncWithDisk();
  TEST_EQUAL(MapOptions::Nothing, localFileV2->GetFiles(), ());

  TEST_EQUAL(TStatus::ENotDownloaded, storage.CountryStatusEx(index), ());
}

UNIT_TEST(StorageTest_DownloadCountryAndDeleteRoutingOnly)
{
  Storage storage;
  if (version::IsSingleMwm(storage.GetCurrentDataVersion()))
    return; // Test on routing mwm is not relevant for single mwm data.

  TaskRunner runner;
  InitStorage(storage, runner);

  TIndex const index = storage.FindIndexByFile("Azerbaijan");
  TEST(IsIndexValid(index), ());
  storage.DeleteCountry(index, MapOptions::MapWithCarRouting);

  {
    unique_ptr<CountryDownloaderChecker> checker =
        AbsentCountryDownloaderChecker(storage, index, MapOptions::MapWithCarRouting);
    checker->StartDownload();
    runner.Run();
  }

  // Delete routing file only and check that latest local file wasn't changed.
  TLocalFilePtr localFileA = storage.GetLatestLocalFile(index);
  TEST(localFileA.get(), ());
  TEST_EQUAL(MapOptions::MapWithCarRouting, localFileA->GetFiles(), ());

  storage.DeleteCountry(index, MapOptions::CarRouting);

  TLocalFilePtr localFileB = storage.GetLatestLocalFile(index);
  TEST(localFileB.get(), ());
  TEST_EQUAL(localFileA.get(), localFileB.get(), (*localFileA, *localFileB));
  TEST_EQUAL(MapOptions::Map, localFileB->GetFiles(), ());

  storage.DeleteCountry(index, MapOptions::Map);
  TLocalFilePtr localFileC = storage.GetLatestLocalFile(index);
  TEST(!localFileC.get(), (*localFileC));
}

UNIT_TEST(StorageTest_DownloadMapAndRoutingSeparately)
{
  Storage storage;
  bool const isSingleMwm = version::IsSingleMwm(storage.GetCurrentDataVersion());
  if (isSingleMwm)
    return;

  TaskRunner runner;
  tests::TestMwmSet mwmSet;
  InitStorage(storage, runner, [&mwmSet](LocalCountryFile const & localFile)
  {
    try
    {
      auto p = mwmSet.Register(localFile);
      TEST(p.first.IsAlive(), ());
    }
    catch (exception & e)
    {
      LOG(LERROR, ("Failed to register:", localFile, ":", e.what()));
    }
  });

  string const mwmName = isSingleMwm ? "Azerbaijan Region" : "Azerbaijan";
  TIndex const index = storage.FindIndexByFile(mwmName);
  TEST(IsIndexValid(index), ());
  CountryFile const countryFile = storage.GetCountryFile(index);

  storage.DeleteCountry(index, MapOptions::Map);

  // Download map file only.
  {
    unique_ptr<CountryDownloaderChecker> checker =
        AbsentCountryDownloaderChecker(storage, index, MapOptions::Map);
    checker->StartDownload();
    runner.Run();
  }

  TLocalFilePtr localFileA = storage.GetLatestLocalFile(index);
  TEST(localFileA.get(), ());
  TEST_EQUAL(MapOptions::Map, localFileA->GetFiles(), ());

  MwmSet::MwmId id = mwmSet.GetMwmIdByCountryFile(countryFile);
  TEST(id.IsAlive(), ());
  TEST_EQUAL(MapOptions::Map, id.GetInfo()->GetLocalFile().GetFiles(), ());

  // Download routing file in addition to exising map file.
  {
    unique_ptr<CountryDownloaderChecker> checker =
        PresentCountryDownloaderChecker(storage, index, MapOptions::CarRouting);
    checker->StartDownload();
    runner.Run();
  }

  TLocalFilePtr localFileB = storage.GetLatestLocalFile(index);
  TEST(localFileB.get(), ());
  TEST_EQUAL(localFileA.get(), localFileB.get(), (*localFileA, *localFileB));
  TEST_EQUAL(MapOptions::MapWithCarRouting, localFileB->GetFiles(), ());

  TEST(id.IsAlive(), ());
  TEST_EQUAL(MapOptions::MapWithCarRouting, id.GetInfo()->GetLocalFile().GetFiles(), ());

  // Delete routing file and check status update.
  {
    CountryStatusChecker checker(storage, index, TStatus::EOnDisk);
    storage.DeleteCountry(index, MapOptions::CarRouting);
  }
  TLocalFilePtr localFileC = storage.GetLatestLocalFile(index);
  TEST(localFileC.get(), ());
  TEST_EQUAL(localFileB.get(), localFileC.get(), (*localFileB, *localFileC));
  TEST_EQUAL(MapOptions::Map, localFileC->GetFiles(), ());

  TEST(id.IsAlive(), ());
  TEST_EQUAL(MapOptions::Map, id.GetInfo()->GetLocalFile().GetFiles(), ());

  // Delete map file and check status update.
  {
    CountryStatusChecker checker(storage, index, TStatus::ENotDownloaded);
    storage.DeleteCountry(index, MapOptions::Map);
  }

  // Framework should notify MwmSet about deletion of a map file.
  // As there're no framework, there should not be any changes in MwmInfo.
  TEST(id.IsAlive(), ());
  TEST_EQUAL(MapOptions::Map, id.GetInfo()->GetLocalFile().GetFiles(), ());
}

UNIT_TEST(StorageTest_DeletePendingCountry)
{
  Storage storage;
  TaskRunner runner;
  InitStorage(storage, runner);

  string const mwmName = version::IsSingleMwm(storage.GetCurrentDataVersion()) ?
      "Azerbaijan Region" : "Azerbaijan";
  TIndex const index = storage.FindIndexByFile(mwmName);
  TEST(IsIndexValid(index), ());
  storage.DeleteCountry(index, MapOptions::Map);

  {
    unique_ptr<CountryDownloaderChecker> checker =
        CancelledCountryDownloaderChecker(storage, index, MapOptions::Map);
    checker->StartDownload();
    storage.DeleteCountry(index, MapOptions::Map);
    runner.Run();
  }
}

UNIT_TEST(StorageTest_DownloadTwoCountriesAndDeleteSingleMwm)
{
  Storage storage;
  if (!version::IsSingleMwm(storage.GetCurrentDataVersion()))
    return;

  TaskRunner runner;
  InitStorage(storage, runner);

  TIndex const uruguayIndex = storage.FindIndexByFile("Uruguay");
  TEST(IsIndexValid(uruguayIndex), ());
  storage.DeleteCountry(uruguayIndex, MapOptions::Map);
  MY_SCOPE_GUARD(cleanupUruguayFiles, bind(&Storage::DeleteCountry, &storage, uruguayIndex,
                                           MapOptions::Map));

  TIndex const venezuelaIndex = storage.FindIndexByFile("Venezuela");
  TEST(IsIndexValid(venezuelaIndex), ());
  storage.DeleteCountry(venezuelaIndex, MapOptions::Map);
  MY_SCOPE_GUARD(cleanupVenezuelaFiles, bind(&Storage::DeleteCountry, &storage, venezuelaIndex,
                                             MapOptions::Map));

  {
    unique_ptr<CountryDownloaderChecker> uruguayChecker = make_unique<CountryDownloaderChecker>(
        storage, uruguayIndex, MapOptions::Map,
        vector<TStatus>{TStatus::ENotDownloaded, TStatus::EDownloading, TStatus::EOnDisk});

    unique_ptr<CountryDownloaderChecker> venezuelaChecker = make_unique<CountryDownloaderChecker>(
        storage, venezuelaIndex, MapOptions::Map,
        vector<TStatus>{TStatus::ENotDownloaded, TStatus::EInQueue,
                        TStatus::EDownloading, TStatus::EOnDisk});

    uruguayChecker->StartDownload();
    venezuelaChecker->StartDownload();
    runner.Run();
  }

  {
    unique_ptr<CountryDownloaderChecker> uruguayChecker = make_unique<CountryDownloaderChecker>(
        storage, uruguayIndex, MapOptions::Map,
        vector<TStatus>{TStatus::EOnDisk, TStatus::ENotDownloaded});

    unique_ptr<CountryDownloaderChecker> venezuelaChecker = make_unique<CountryDownloaderChecker>(
        storage, venezuelaIndex, MapOptions::Map,
        vector<TStatus>{TStatus::EOnDisk, TStatus::ENotDownloaded});

    storage.DeleteCountry(uruguayIndex, MapOptions::Map);
    storage.DeleteCountry(venezuelaIndex, MapOptions::Map);
    runner.Run();
  }

  TLocalFilePtr uruguayFile = storage.GetLatestLocalFile(uruguayIndex);
  TEST(!uruguayFile.get(), (*uruguayFile));

  TLocalFilePtr venezuelaFile = storage.GetLatestLocalFile(venezuelaIndex);
  TEST(!venezuelaFile.get(), ());
}

UNIT_TEST(StorageTest_DownloadTwoCountriesAndDeleteTwoComponentMwm)
{
  Storage storage;
  if (version::IsSingleMwm(storage.GetCurrentDataVersion()))
    return;

  TaskRunner runner;
  InitStorage(storage, runner);

  TIndex const uruguayIndex = storage.FindIndexByFile("Uruguay");
  TEST(IsIndexValid(uruguayIndex), ());
  storage.DeleteCountry(uruguayIndex, MapOptions::MapWithCarRouting);
  MY_SCOPE_GUARD(cleanupUruguayFiles, bind(&Storage::DeleteCountry, &storage, uruguayIndex,
                                           MapOptions::MapWithCarRouting));

  TIndex const venezuelaIndex = storage.FindIndexByFile("Venezuela");
  TEST(IsIndexValid(venezuelaIndex), ());
  storage.DeleteCountry(venezuelaIndex, MapOptions::MapWithCarRouting);
  MY_SCOPE_GUARD(cleanupVenezuelaFiles, bind(&Storage::DeleteCountry, &storage, venezuelaIndex,
                                             MapOptions::MapWithCarRouting));

  {
    // Map file will be deleted for Uruguay, thus, routing file should also be deleted. Therefore,
    // Uruguay should pass through following states: NotDownloaded -> Downloading -> NotDownloaded.
    unique_ptr<CountryDownloaderChecker> uruguayChecker = make_unique<CountryDownloaderChecker>(
        storage, uruguayIndex, MapOptions::MapWithCarRouting,
        vector<TStatus>{TStatus::ENotDownloaded, TStatus::EDownloading, TStatus::ENotDownloaded});
    // Only routing file will be deleted for Venezuela, thus, Venezuela should pass through
    // following
    // states:
    // NotDownloaded -> InQueue (Venezuela is added after Uruguay) -> Downloading -> Downloading
    // (second notification will be sent after deletion of a routing file) -> OnDisk.
    unique_ptr<CountryDownloaderChecker> venezuelaChecker = make_unique<CountryDownloaderChecker>(
        storage, venezuelaIndex, MapOptions::MapWithCarRouting,
        vector<TStatus>{TStatus::ENotDownloaded, TStatus::EInQueue, TStatus::EDownloading,
                        TStatus::EDownloading, TStatus::EOnDisk});
    uruguayChecker->StartDownload();
    venezuelaChecker->StartDownload();
    storage.DeleteCountry(uruguayIndex, MapOptions::Map);
    storage.DeleteCountry(venezuelaIndex, MapOptions::CarRouting);
    runner.Run();
  }
  // @TODO(bykoianko) This test changed its behaivier. This commented lines are left specially
  // to fixed later.
  TLocalFilePtr uruguayFile = storage.GetLatestLocalFile(uruguayIndex);
  TEST(!uruguayFile.get(), (*uruguayFile));

  TLocalFilePtr venezuelaFile = storage.GetLatestLocalFile(venezuelaIndex);
  TEST(venezuelaFile.get(), ());
  TEST_EQUAL(MapOptions::Map, venezuelaFile->GetFiles(), ());
}

UNIT_TEST(StorageTest_CancelDownloadingWhenAlmostDone)
{
  Storage storage;
  TaskRunner runner;
  InitStorage(storage, runner);

  TIndex const index = storage.FindIndexByFile("Uruguay");
  TEST(IsIndexValid(index), ());
  storage.DeleteCountry(index, MapOptions::Map);
  MY_SCOPE_GUARD(cleanupFiles,
                 bind(&Storage::DeleteCountry, &storage, index, MapOptions::Map));

  {
    CancelDownloadingWhenAlmostDoneChecker checker(storage, index, runner);
    checker.StartDownload();
    runner.Run();
  }
  TLocalFilePtr file = storage.GetLatestLocalFile(index);
  TEST(!file, (*file));
}

UNIT_TEST(StorageTest_DeleteCountry)
{
  Storage storage;
  TaskRunner runner;
  InitStorage(storage, runner);

  tests_support::ScopedFile map("Wonderland.mwm", "map");
  LocalCountryFile file = LocalCountryFile::MakeForTesting("Wonderland");
  TEST_EQUAL(MapOptions::Map, file.GetFiles(), ());

  CountryIndexes::PreparePlaceOnDisk(file);
  string const bitsPath = CountryIndexes::GetPath(file, CountryIndexes::Index::Bits);
  {
    FileWriter writer(bitsPath);
    string const data = "bits";
    writer.Write(data.data(), data.size());
  }

  storage.RegisterFakeCountryFiles(file);
  TEST(map.Exists(), ());
  TEST(Platform::IsFileExistsByFullPath(bitsPath), (bitsPath));

  storage.DeleteCustomCountryVersion(file);
  TEST(!map.Exists(), ())
  TEST(!Platform::IsFileExistsByFullPath(bitsPath), (bitsPath));

  map.Reset();
}

UNIT_TEST(StorageTest_FailedDownloading)
{
  Storage storage;
  storage.Init(&OnCountryDownloaded);
  storage.SetDownloaderForTesting(make_unique<TestMapFilesDownloader>());
  storage.SetCurrentDataVersionForTesting(1234);

  TIndex const index = storage.FindIndexByFile("Uruguay");
  CountryFile const countryFile = storage.GetCountryFile(index);

  // To prevent interference from other tests and on other tests it's
  // better to remove temprorary downloader files.
  DeleteDownloaderFilesForCountry(countryFile, storage.GetCurrentDataVersion());
  MY_SCOPE_GUARD(cleanup, [&]()
  {
    DeleteDownloaderFilesForCountry(countryFile, storage.GetCurrentDataVersion());
  });

  {
    FailedDownloadingWaiter waiter(storage, index);
    storage.DownloadCountry(index, MapOptions::Map);
    QCoreApplication::exec();
  }

  // File wasn't downloaded, but temprorary downloader files must exist.
  string const downloadPath =
      GetFileDownloadPath(countryFile, MapOptions::Map, storage.GetCurrentDataVersion());
  TEST(!Platform::IsFileExistsByFullPath(downloadPath), ());
  TEST(Platform::IsFileExistsByFullPath(downloadPath + DOWNLOADING_FILE_EXTENSION), ());
  TEST(Platform::IsFileExistsByFullPath(downloadPath + RESUME_FILE_EXTENSION), ());
}

// "South Georgia and the South Sandwich" doesn't have roads, so there
// is no routing file for this island.
UNIT_TEST(StorageTest_EmptyRoutingFile)
{
  Storage storage;
  if (version::IsSingleMwm(storage.GetCurrentDataVersion()))
    return; // Following code tests car routing files, and is not relevant for a single mwm case.

  TaskRunner runner;
  InitStorage(storage, runner, [](LocalCountryFile const & localFile)
              {
                TEST_EQUAL(localFile.GetFiles(), MapOptions::Map, ());
              });

  TIndex const index = storage.FindIndexByFile("South Georgia and the South Sandwich Islands");
  TEST(IsIndexValid(index), ());
  storage.DeleteCountry(index, MapOptions::MapWithCarRouting);
  MY_SCOPE_GUARD(cleanup,
                 bind(&Storage::DeleteCountry, &storage, index, MapOptions::MapWithCarRouting));

  CountryFile const country = storage.GetCountryFile(index);
  TEST_NOT_EQUAL(country.GetRemoteSize(MapOptions::Map), 0, ());
  TEST_EQUAL(country.GetRemoteSize(MapOptions::CarRouting), 0, ());

  auto checker = AbsentCountryDownloaderChecker(storage, index, MapOptions::MapWithCarRouting);
  checker->StartDownload();
  runner.Run();
}

UNIT_TEST(StorageTest_ObsoleteMapsRemoval)
{
  Storage storage;
  CountryFile country("Azerbaijan Region");

  tests_support::ScopedDir dir1("1");
  tests_support::ScopedFile map1(dir1, country, MapOptions::Map, "map1");
  LocalCountryFile file1(dir1.GetFullPath(), country, 1 /* version */);
  CountryIndexes::PreparePlaceOnDisk(file1);

  tests_support::ScopedDir dir2("2");
  tests_support::ScopedFile map2(dir2, country, MapOptions::Map, "map2");
  LocalCountryFile file2(dir2.GetFullPath(), country, 2 /* version */);
  CountryIndexes::PreparePlaceOnDisk(file2);

  TEST(map1.Exists(), ());
  TEST(map2.Exists(), ());

  storage.RegisterAllLocalMaps();

  TEST(!map1.Exists(), ());
  map1.Reset();

  TEST(map2.Exists(), ());
}
}  // namespace storage
