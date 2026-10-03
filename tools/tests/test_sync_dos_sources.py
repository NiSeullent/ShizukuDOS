# SPDX-License-Identifier: GPL-2.0-only
"""Actual small Git repositories; no network, compiler, guest or private input."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("dos_sync", Path(__file__).resolve().parents[1]/"sync_dos_sources.py")
sync = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sync)
PATH = "shizukudos/boot.asm"
SECOND = "shizukudos/stage2.asm"

class SyncTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        base = Path(self.temp.name)
        self.source, self.target = base/"source", base/"target"
        for repo in (self.source, self.target):
            repo.mkdir()
            self.git(repo,"init","-q")
            self.git(repo,"config","user.name","Source Fixture")
            self.git(repo,"config","user.email","fixture@example.invalid")
            (repo/"shizukudos").mkdir()
            for path in (PATH,SECOND):
                (repo/path).write_bytes(b"; shared original\n")
        self.git(self.source,"remote","add","origin",sync.UPSTREAM+".git")
        self.commit(self.source)
        (self.target/"docs").mkdir()
        (self.target/"SOURCE_ORIGIN.json").write_bytes(b'{"initial":"preserved"}\n')
        self.origin=(self.target/"SOURCE_ORIGIN.json").read_bytes()
        self.policy={"schema":sync.SCHEMA,"upstream_repository":sync.UPSTREAM,
                     "source_origin_sha256":sync.sha(self.origin),
                     "files":{p:{"baseline_sha256":sync.sha(b"; shared original\n")} for p in (PATH,SECOND)}}
        self.write_policy()
        self.commit(self.target)
    def git(self,repo,*args):
        return subprocess.check_output(["git","-C",str(repo),*args],stderr=subprocess.PIPE,timeout=10)
    def commit(self,repo):
        self.git(repo,"add","-A");self.git(repo,"commit","-qm","fixture")
        return self.git(repo,"rev-parse","HEAD").decode().strip()
    def write_policy(self):
        (self.target/sync.POLICY).write_text(json.dumps(self.policy,sort_keys=True)+"\n")
    def head(self):
        return self.git(self.source,"rev-parse","HEAD").decode().strip()
    def handoff(self,**changes):
        d={"owner":"fixture-owner","approved":True,"source_commit":self.head(),"paths":[PATH,SECOND]}
        d.update(changes)
        path=Path(self.temp.name)/"reviewed.json";path.write_text(json.dumps(d));return path
    def plan(self,**kwargs):
        return sync.plan_sync(self.target,self.source,self.head(),**kwargs)
    def change(self,repo,path,data):
        (repo/path).write_bytes(data);self.commit(repo)
    def before(self):
        return {p:(self.target/p).read_bytes() for p in (PATH,SECOND,sync.POLICY,"SOURCE_ORIGIN.json")}
    def test_compare_is_deterministic_and_has_zero_file_writes(self):
        before=self.before();a,_=self.plan();b,_=self.plan()
        self.assertEqual(a,b);self.assertTrue(all(e['status']=='unchanged' for e in a['entries']))
        self.assertEqual(before,self.before());self.assertFalse(a['applied'])
    def test_selected_source_only_apply_updates_ledger_and_preserves_origin(self):
        self.change(self.source,PATH,b"; reviewed update\n")
        report,state=self.plan(paths=[PATH],handoff=self.handoff())
        self.assertEqual(report['entries'][0]['status'],'source_only')
        sync.apply_sync(report,state)
        self.assertEqual((self.target/PATH).read_bytes(),b"; reviewed update\n")
        self.assertEqual((self.target/SECOND).read_bytes(),b"; shared original\n")
        ledger=json.loads((self.target/sync.POLICY).read_text())
        self.assertEqual(ledger['files'][PATH]['baseline_sha256'],sync.sha(b"; reviewed update\n"))
        self.assertEqual((self.target/'SOURCE_ORIGIN.json').read_bytes(),self.origin)
        self.assertEqual(ledger['last_reviewed_sync']['source_commit'],self.head())
    def test_target_only_change_is_preserved(self):
        self.change(self.target,PATH,b"; Core private development\n")
        report,state=self.plan(handoff=self.handoff())
        self.assertEqual(report['entries'][0]['status'],'target_only')
        sync.apply_sync(report,state)
        self.assertEqual((self.target/PATH).read_bytes(),b"; Core private development\n")
        self.assertEqual(json.loads((self.target/sync.POLICY).read_text())['files'][PATH]['baseline_sha256'],sync.sha(b"; shared original\n"))
    def test_both_sides_changed_refuses_all_writes(self):
        self.change(self.target,PATH,b"; target independent\n")
        self.change(self.source,PATH,b"; source independent\n")
        self.change(self.source,SECOND,b"; source second\n")
        report,state=self.plan(handoff=self.handoff());before=self.before()
        self.assertIn('conflict',[e['status'] for e in report['entries']])
        with self.assertRaisesRegex(sync.SyncRefused,'both sides changed'):sync.apply_sync(report,state)
        self.assertEqual(before,self.before())
    def test_identical_independent_changes_converge(self):
        for repo in (self.source,self.target):self.change(repo,PATH,b"; same new bytes\n")
        report,state=self.plan(handoff=self.handoff());self.assertEqual(report['entries'][0]['status'],'converged')
        sync.apply_sync(report,state)
        self.assertEqual(json.loads((self.target/sync.POLICY).read_text())['files'][PATH]['baseline_sha256'],sync.sha(b"; same new bytes\n"))
    def test_no_owner_review_refuses_writes(self):
        report,state=self.plan();before=self.before()
        with self.assertRaisesRegex(sync.SyncRefused,'handoff required'):sync.apply_sync(report,state)
        self.assertEqual(before,self.before())
    def test_mismatched_handoff_refused(self):
        for changes in ({'approved':False},{'source_commit':'0'*40},{'owner':''},{'paths':[]},{'paths':[PATH,'vm/private.img']},{'paths':[[PATH]]},{'unrelated_secret':'refused extra field'}):
            with self.subTest(changes=changes),self.assertRaises(sync.SyncRefused):self.plan(handoff=self.handoff(**changes))
    def test_wrong_source_origin_refused(self):
        self.git(self.source,'remote','set-url','origin','https://example.invalid/other.git')
        with self.assertRaisesRegex(sync.SyncRefused,'checkout origin'):self.plan()
    def test_full_source_commit_required(self):
        for ref in ('HEAD',self.head()[:10]):
            with self.assertRaisesRegex(sync.SyncRefused,'complete immutable'):sync.plan_sync(self.target,self.source,ref)
    def test_path_allowlist_refuses_non_dos_and_traversal(self):
        for path in ('SOURCE_ORIGIN.json','shizukudos/kernel64/main.c','../secret','shizukudos/dos16/user/RECOVER.BAT'):
            with self.subTest(path=path),self.assertRaisesRegex(sync.SyncRefused,'selected path'):self.plan(paths=[path])
    def test_policy_cannot_expand_allowlist(self):
        self.policy['files']['shizukudos/kernel64/syscall.c']={'baseline_sha256':'0'*64};self.write_policy()
        with self.assertRaisesRegex(sync.SyncRefused,'policy path'):self.plan()
    def test_initial_origin_change_refused(self):
        (self.target/'SOURCE_ORIGIN.json').write_bytes(b'{"replaced":true}')
        with self.assertRaisesRegex(sync.SyncRefused,'initial source origin'):self.plan()
    def test_source_symlink_refused(self):
        (self.source/PATH).unlink();(self.source/PATH).symlink_to('stage2.asm');self.commit(self.source)
        with self.assertRaisesRegex(sync.SyncRefused,'regular Git blob'):self.plan()
    def test_source_binary_and_size_refused(self):
        for data in (b'; bad\0binary',b'a'*(sync.MAX_BLOB+1)):
            self.change(self.source,PATH,data)
            with self.assertRaises(sync.SyncRefused):self.plan()
    def test_source_deletion_refused(self):
        (self.source/PATH).unlink();self.commit(self.source)
        with self.assertRaisesRegex(sync.SyncRefused,'missing exact source'):self.plan()
    def test_worktree_symlink_refused(self):
        (self.target/PATH).unlink();(self.target/PATH).symlink_to('stage2.asm')
        with self.assertRaisesRegex(sync.SyncRefused,'symlink'):self.plan()
    def test_target_dirty_refused_before_write(self):
        report,state=self.plan(handoff=self.handoff());before=self.before()
        (self.target/'unreviewed.txt').write_text('concurrent change')
        with self.assertRaisesRegex(sync.SyncRefused,'clean target'):sync.apply_sync(report,state)
        self.assertEqual(before,self.before())
    def test_target_commit_drift_refused(self):
        report,state=self.plan(handoff=self.handoff());before=self.before()
        (self.target/'new.txt').write_text('committed concurrent change');self.commit(self.target)
        with self.assertRaisesRegex(sync.SyncRefused,'target commit changed'):sync.apply_sync(report,state)
        self.assertEqual(before,self.before())
    def test_policy_and_file_drift_refuse_even_ignored_status(self):
        for path in (PATH,sync.POLICY):
            with self.subTest(path=path):
                report,state=self.plan(handoff=self.handoff());before=self.before()
                (self.target/path).write_bytes(before[path]+b'\n')
                real_git=sync.git
                def fake_git(repo,*args):return b'' if args[0]=='status' else real_git(repo,*args)
                with patch.object(sync,'git',fake_git),self.assertRaisesRegex(sync.SyncRefused,'changed after planning'):
                    sync.apply_sync(report,state)
                for name,data in before.items():(self.target/name).write_bytes(data)
    def test_write_failure_rolls_back_selected_files_and_ledger(self):
        self.change(self.source,PATH,b'; accepted first\n');self.change(self.source,SECOND,b'; accepted second\n')
        report,state=self.plan(handoff=self.handoff());before=self.before();real=sync._replace;calls=0
        def failing(path,data):
            nonlocal calls
            calls+=1
            if calls==2:raise OSError('fixture write failure')
            return real(path,data)
        with patch.object(sync,'_replace',failing),self.assertRaisesRegex(OSError,'write failure'):
            sync.apply_sync(report,state)
        self.assertEqual(before,self.before())
    def assert_unreplaced_plan(self, commit):
        report,state=sync.plan_sync(self.target,self.source,commit,paths=[PATH],handoff=self.handoff(source_commit=commit))
        self.assertEqual(report['source_commit'],commit)
        self.assertEqual(report['entries'][0]['source_sha256'],sync.sha(b"; shared original\n"))
        self.assertEqual(state[4][PATH],b"; shared original\n")
        sync.apply_sync(report,state)
        self.assertEqual((self.target/PATH).read_bytes(),b"; shared original\n")
    def test_commit_replace_cannot_change_approved_commit_contents(self):
        approved=self.head()
        self.change(self.source,PATH,b"; unapproved replacement commit\n")
        replacement=self.head();self.git(self.source,'replace',approved,replacement)
        self.assert_unreplaced_plan(approved)
    def test_blob_replace_cannot_change_recorded_blob_contents(self):
        approved=self.head()
        original=self.git(self.source,'rev-parse',approved+':'+PATH).decode().strip()
        self.change(self.source,PATH,b"; unapproved replacement blob\n")
        replacement=self.git(self.source,'rev-parse',self.head()+':'+PATH).decode().strip()
        self.git(self.source,'replace',original,replacement)
        self.assert_unreplaced_plan(approved)
    def test_alternate_replace_ref_base_is_ignored(self):
        approved=self.head()
        self.change(self.source,PATH,b"; alternate replacement commit\n")
        self.git(self.source,'update-ref','refs/sync-custom-replacements/'+approved,self.head())
        with patch.dict(os.environ,{'GIT_REPLACE_REF_BASE':'refs/sync-custom-replacements/','GIT_NO_REPLACE_OBJECTS':'0'}):
            self.assert_unreplaced_plan(approved)
    def test_environment_config_cannot_spoof_source_origin(self):
        self.git(self.source,'remote','set-url','origin','https://example.invalid/unreviewed.git')
        influences=(
            {'GIT_CONFIG_COUNT':'1','GIT_CONFIG_KEY_0':'remote.origin.url','GIT_CONFIG_VALUE_0':sync.UPSTREAM},
            {'GIT_CONFIG_PARAMETERS':"'remote.origin.url'='"+sync.UPSTREAM+"'"})
        for values in influences:
            with self.subTest(values=values),patch.dict(os.environ,values),self.assertRaisesRegex(sync.SyncRefused,'checkout origin'):
                self.plan()
    def test_included_or_global_config_cannot_spoof_source_origin(self):
        self.git(self.source,'config','--unset','remote.origin.url')
        config=Path(self.temp.name)/'injected-config'
        config.write_text(chr(10).join(('[remote "origin"]', "url = "+sync.UPSTREAM, "")))
        self.git(self.source,'config','include.path',str(config))
        self.assertEqual(self.git(self.source,'config','--get','remote.origin.url').decode().strip(),sync.UPSTREAM)
        with patch.dict(os.environ,{'GIT_CONFIG_GLOBAL':str(config)}),self.assertRaises(sync.SyncRefused):self.plan()
    def test_status_never_executes_configured_fsmonitor_hook(self):
        marker=Path(self.temp.name)/'fsmonitor-executed'
        hook=Path(self.temp.name)/'fsmonitor-hook'
        hook.write_text(chr(10).join(("#!/bin/sh", 'printf executed > "'+str(marker)+'"', "")))
        hook.chmod(0o700)
        self.git(self.target,'config','core.fsmonitor',str(hook))
        self.git(self.target,'status','--porcelain=v1')
        self.assertTrue(marker.exists())
        marker.unlink()
        report,state=self.plan(handoff=self.handoff())
        sync.apply_sync(report,state)
        self.assertFalse(marker.exists())
    def test_missing_promised_blob_never_launches_remote_transport(self):
        # A real partial clone contains the commit/tree but not its file blob.
        # Its fake promisor transport only writes a local marker and fails.
        self.git(self.source,'config','uploadpack.allowFilter','true')
        partial=Path(self.temp.name)/'partial'
        self.git(self.source,'clone','--quiet','--filter=blob:none','--no-checkout',
                 self.source.as_uri(),str(partial))
        oid=self.git(self.source,'rev-parse',self.head()+':'+PATH).decode().strip()
        missing=subprocess.run(['git','--no-lazy-fetch','-C',str(partial),
                                'cat-file','-e',oid],stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE,timeout=10)
        self.assertNotEqual(missing.returncode,0)
        marker=Path(self.temp.name)/'transport-executed'
        hook=Path(self.temp.name)/'promisor-transport'
        hook.write_text('#!/bin/sh\nprintf executed > "'+str(marker)+'"\nexit 1\n')
        hook.chmod(0o700)
        self.git(partial,'config','protocol.ext.allow','always')
        self.git(partial,'remote','set-url','origin','ext::'+str(hook))
        before=self.before()
        # Even a caller trying to enable lazy fetch must not override the helper.
        with patch.dict(os.environ,{'GIT_NO_LAZY_FETCH':'0'}),self.assertRaises(sync.SyncRefused):
            sync.git(partial,'cat-file','blob',oid)
        self.assertFalse(marker.exists(),'read-only helper launched a promisor transport')
        self.assertEqual(before,self.before())
    def test_shared_source_baseline_advances_for_next_update(self):
        self.change(self.source,PATH,b'; first reviewed\n')
        report,state=self.plan(handoff=self.handoff());sync.apply_sync(report,state);self.commit(self.target)
        self.change(self.source,PATH,b'; second reviewed\n')
        report,state=self.plan(handoff=self.handoff());self.assertEqual(report['entries'][0]['status'],'source_only')
        sync.apply_sync(report,state);self.assertEqual((self.target/PATH).read_bytes(),b'; second reviewed\n')

if __name__=='__main__':unittest.main()
