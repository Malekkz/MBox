# Conformance tests of the tag database

Where each conformance test of the tag database contract
(`docs/contracts/tag-database.md`, section 15) is automated. The test
`scada-conformance-index` checks this table on every build: every test of
section 15 has a row with at least one test, every test named here exists,
and every test whose name carries `Conformance<NN>` is named in row NN. A
new conformance test in the contract therefore fails the build until it is
automated and listed here.

A name is a GoogleTest test, `Suite.Name`, or a CTest test, `ctest:<name>`.
"Pipeline" says where it runs: "both" is the `dev` and `release` tests of
`build and test (x86-64)` and `build and test (ARM64)`; every test of the
`dev` presets also runs in `sanitizers (asan-ubsan)` and `sanitizers
(tsan)` on x86-64. `root` is `sudo ctest --preset root` in both `build and
test` jobs. `measurements` is the manual and weekly job of
`.github/workflows/measurements.yml`.

| Test | Pipeline | Automated by |
| --- | --- | --- |
| 1 | both | `Conformance01Layout.ChangeEntry`, `Conformance01Layout.ChangeRingHeader`, `Conformance01Layout.CursorArrayFollowsProcHeader`, `Conformance01Layout.DefaultValueIsAllZero`, `Conformance01Layout.DirectoryEntry`, `Conformance01Layout.EventRecord`, `Conformance01Layout.GlobalHeader`, `Conformance01Layout.OwnerEntry`, `Conformance01Layout.ProcHeader`, `Conformance01Layout.ProcessEntry`, `Conformance01Layout.QueueEntry`, `Conformance01Layout.QueueHeader`, `Conformance01Layout.RegionHeader`, `Conformance01Layout.RegionMagicSpellsItsName`, `Conformance01Layout.StringSlot`, `Conformance01Layout.TagRecord`, `Conformance01Layout.WriteRequest`, `Conformance01Layout.WriteResult` |
| 2 | both; 3 s per run, 600 s in `measurements` | `TagAccessProcesses.Conformance02ReferenceLoadWithFourReaders` |
| 3 | both | `TagAccess.Conformance03AReaderNeverSeesFieldsOfTwoWrites`, `TagAccessProcesses.Conformance03MaximumWriteRateWithFourReaders` |
| 4 | herd7: x86-64 (`model check`); code shape: ARM64 release | `ctest:scada-model-check`, `ctest:scada-model-code-shape-test`, `ctest:scada-model-code-shape-mutants-test` |
| 5 | both | `Host.Conformance05EveryProcessIsDownUntilItAttaches`, `Host.Conformance05ProcessIsDownOnceItsHeartbeatIsOlderThanItsTimeout`, `Host.Conformance05RestartedProcessIsUpAgain`, `TagAccess.Conformance05ARecordLeftMidWriteByADeadOwnerReadsAsDown`, `TagAccessProcesses.Conformance05AKilledOwnerReadsAsDownWithinItsTimeout`, `TagAccessProcesses.Conformance05AKilledOwnerReadsAsDownWithoutTagd`, `TagdService.Conformance05KilledProcessIsMarkedDown`, `TagdService.Conformance05RestartedProcessIsMarkedUpAgain` |
| 6 | both | `Host.Conformance06CleanStopIsMarkedAtTheNextCheck`, `TagAccess.Conformance06AnOwnerThatStopsCleanlyReadsAsDownAtOnce`, `TagAccessProcesses.Conformance06ACleanlyStoppedOwnerReadsAsDownAtOnce`, `TagdService.Conformance06CleanlyStoppedProcessIsMarkedDown` |
| 7 | both | `Attach.Conformance07AnOwnerRestartRepairsItsRegion`, `Attach.Conformance07EveryRestartOfAnOwnerIncrementsTheEpochOfItsRing`, `ChangeRingProcesses.Conformance07AKilledOwnerThatRestartsGivesOneGap`, `Changes.Conformance07AnOwnerRestartGivesEveryConsumerOneGap`, `TagAccessProcesses.Conformance07AReaderRecoversWhenTheOwnerRestarts`, `TagdService.Conformance07AKilledOwnerRepairsItsRegionOnRestart`, `TagdService.Conformance07ARestartAfterACleanStopRepairsNothing` |
| 8 | both | `TagAccess.Conformance08AProcessThatIsNotTheOwnerCannotWrite` |
| 9 | both | `Attach.Conformance09ABaseDirectoryWithTheWrongOwnerOrModeIsRefused`, `Attach.Conformance09ASymbolicLinkUnderARegionNameIsNotFollowed`, `HeaderFile.Conformance09AHeaderOfAnotherUserIsRefused`, `HeaderFile.Conformance09ASymbolicLinkIsNotFollowed`, `Host.Conformance09BaseDirectoryWithWrongOwnerOrModeIsRefused`, `Host.Conformance09SweepRemovesUnlistedEntriesAndThoseOfTheWrongUser`, `Peers.Conformance09ARegionCreatedByTheWrongUserIsRejected`, `Peers.Conformance09ASymbolicLinkUnderARegionNameIsNotFollowed`, `RegionFile.Conformance09SymlinkUnderARegionNameIsNotFollowed` |
| 10 | both | `Attach.Conformance10AProcessKilledWhileCreatingARegionAttachesAgain`, `Attach.Conformance10ASecondProcessOfTheSameNameIsRefused`, `ProcRegion.Conformance10AttachesAfterAnInterruptedCreation`, `ProcRegion.Conformance10OwnUnusableRegionIsRebuilt`, `ProcRegion.Conformance10SecondAttachWithTheSameNameIsRefused`, `TagdService.Conformance10ASecondProcessOfTheSameNameIsRefused` |
| 11 | both; the full file system: `root`, both | `Attach.Conformance11RegionsGetMode0640UnderUmask077`, `FullShm.Conformance11AttachToAFullFileSystemFailsWithAnError`, `Host.Conformance11GenerationHasTheModesOfTheContractUnderUmask077`, `Host.Conformance11HeaderThatCannotBeAllocatedFailsWithAnError`, `RegionFile.Conformance11RegionHasMode0640UnderUmask077` |
| 12 | both | `Attach.Conformance12AHeaderOfAnotherLayoutVersionIsRefused`, `CApi.Conformance12AnotherLayoutVersionRefusesToAttach`, `ReadHeader.Conformance12AnotherLayoutVersionIsRefused` |
| 13 | both | `ChangeRingProcesses.Conformance13AFastConsumerSeesEveryEntryInOrder`, `Changes.Conformance13AConsumerJustInsideTheRingGetsNoGap`, `Changes.Conformance13AFastConsumerSeesEveryEntryInOrder`, `Changes.Conformance13ASlowConsumerGetsAGapExactlyWhenOverrun` |
| 14 | both | `Changes.Conformance14AnAvalancheGivesNoGapToAConsumerThatKeepsUp` |
| 15 | both; the race check: `sanitizers (tsan)`, x86-64 | `OwnerThreads.Conformance15OwnerThreadsWriteAndPostWithoutARace` |
| 16 | both | `QueueProcesses.Conformance16RedeliveryAfterAConsumerIsKilled`, `Queues.EventsArriveInOrderOnceEachWithTheirPositionAsId`, `Queues.APostToAFullQueueIsRefusedUntilARecordIsAcknowledged`, `Queues.ARecordNotAcknowledgedBeforeARestartIsDeliveredAgain`, `Queues.APostIsRefusedWhileTheConsumerIsDown`, `Queues.AConsumerWithoutARegionIsDown`, `QueueProcesses.OneMillionRecordsWithoutLossDuplicationOrReordering` |
| 17 | both | `QueueProcesses.Conformance17ARequestIsNeverExecutedTwice`, `Queues.ARequestTakenIsNeverDeliveredAgain`, `Queues.RequestsQueuedBeforeTheDriverAttachesAreAnsweredAborted`, `Queues.AnExpiredOrOverLongRequestIsAnsweredExpired` |
| 18 | both | `Queues.IdsContinueAfterAProducerRestart`, `Queues.ACursorOutOfRangeIsRejectedOnBothSides`, `Queues.AWriteSeqTooFarAheadIsRejected` |
| 19 | both | `Queues.APostIsRefusedWhileTheConsumerIsDown`, `Queues.AConsumerWithoutARegionIsDown`, `QueueProcesses.Conformance16RedeliveryAfterAConsumerIsKilled` |
| 20 | both | `Host.Conformance20RestartWithTheSameTagSetReusesTheGeneration`, `Host.Conformance20RunningProcessStaysUpAcrossTheRestart`, `TagdService.Conformance20RestartLeavesTheOtherProcessRunning`, `TagdService.Conformance20RestartWithTheSameTagSetReusesTheGeneration` |
| 21 | both | `Retained.Conformance21AValueWrittenRightBeforeTheChangeIsRestored`, `RetiringQueues.Conformance21NoEventRecordIsLostAcrossAGenerationChange`, `RetiringQueues.Conformance21NoRequestOfTheOldGenerationIsExecutedInTheNewOne`, `Keeper.AnOwnerThatLeavesARetiredGenerationGetsItsFinalFileThenItIsRemoved` |
| 22 | both, native builds without a sanitizer | `RealTimeSafety.Conformance22NoSystemCallAllocationOrPageFault` |
| 23 | both | `TagAccess.Conformance23AStringIsReadWithTheQualityOfItsWrite` |
| 24 | both | `ChangeRingProcesses.Conformance24TheDescriptorAndTheCycleWorkAcross`, `Changes.Conformance24AWaitSetOverTenRingsWakesForAnyOfThem`, `Changes.Conformance24AWaiterOnTheCycleWakesWithoutATagChange`, `Changes.Conformance24NotifyWakesABlockedConsumer`, `Changes.Conformance24TheDescriptorOfAWaitSetWorksWithPoll`, `Changes.Conformance24WithoutNotifyAChangeArrivesWithinTheCap` |
| 25 | both | `Restore.Conformance25ACorruptOrTruncatedFileIsIgnoredAndAttachSucceeds`, `Retained.Conformance25ADamagedFileIsIgnoredAndReportedAndAttachSucceeds`, `Retained.Conformance25ARetainNowTagIsOnDiskWithinOneSecond` |
| 26 | `root`, both, dev and release; 10 s per run, 600 s in `measurements` | `FalseTrips.Conformance26RealTimeReadersBesideTheOwnersTripNothing` |
| 27 | the `perf` gate of both `build and test` jobs, and `measurements` | `Latency.Conformance27ReadAndWriteLatencyUnderTheReferenceLoad` |
| 28 | both; the users part: `root`, both | `Observer.OpensWithoutAProcessEntry`, `Observer.LeavesTheGenerationDirectoryAsItWas`, `Observer.MapsOnlyReadOnly`, `Observer.RefusesWrites`, `Observer.HasNoQueue`, `Observer.SeveralRunBesideALiveAttachOfTheSameTagSet`, `Observer.ReportsARetiringGeneration`, `Observer.ReportsACurrentThatNamesAnotherGeneration`, `ObserverProcesses.SeveralObserveALiveWriterBesideAnAttach`, `ObserverProcesses.MapsOnlyReadOnlyAndReleasesEverything`, `ObserverProcesses.FollowsAGenerationChangeByObservingAgain`, `ObserverUsers.AUserOutsideTheGroupIsRefused`, `ObserverUsers.AGroupMemberOutsideTheProcessTableObserves` |
| 29 | both | `QueueStress.RandomKillsAndRecreationsLoseNothingSilently`, `Queues.TagdReportsAQueueFileCreatedAnew`, `Queues.AQueueFileRemovedUnderARunningProducerStillDelivers`, `Queues.AReplacedQueueFileLosesNothingAlsoWithTheCursorAtZero`, `Queues.TheProducerKeepsItsHighWaterMark`, `Queues.AnIdBurnedByACrashIsNotGivenAgain`, `Queues.TheMarkIsStoredBeforeTheRecord`, `Queues.ANewFileWithoutABaseKillsTheQueue`, `Queues.ANewFileWithABaseBelowTheOldWriteSeqKillsTheQueue`, `BrokenNewFile.ABaseBeyondWriteSeqIsRefused`, `BrokenNewFile.AnAllOnesBaseIsRefused`, `BrokenNewFile.AFlagBitThatIsNoneIsRefused`, `BrokenNewFile.AReservedFieldThatIsNotZeroIsRefused`, `ReplacedQueue.ATruncatedQueueFileKillsTheQueue`, `Queues.TheSeventeenthFileUnderANameStopsTheQueue` |
