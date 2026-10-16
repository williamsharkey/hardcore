#!/usr/bin/env python3
"""Offline updater checks against the real signed release archive."""
import hashlib
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest import mock
import zipfile

spec=importlib.util.spec_from_file_location('update_mac',Path('tools/update_mac.py'))
updater=importlib.util.module_from_spec(spec);spec.loader.exec_module(updater)


class UpdateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.archive=Path('build/hardcore-macos.zip').read_bytes()
        cls.metadata={'tag_name':'v99.0','assets':[{'name':'hardcore-macos.zip',
            'digest':'sha256:'+hashlib.sha256(cls.archive).hexdigest(),
            'browser_download_url':'https://example.invalid/release.zip'}]}

    def fetch(self,url,limit=updater.MAX_DOWNLOAD):
        return json.dumps(self.metadata).encode() if url==updater.RELEASE_URL else self.archive

    def test_verified_install_and_failed_rename_rollback(self):
        with tempfile.TemporaryDirectory() as tmp:
            app=Path(tmp)/'hardcore.app';shutil.copytree('hardcore.app',app)
            exe=app/'Contents/MacOS/hardcore'
            marker=app/'previous-marker';marker.write_text('original')
            with mock.patch.object(updater,'fetch',side_effect=self.fetch):
                with mock.patch.object(updater.os,'rename',wraps=updater.os.rename) as rename:
                    real_rename=rename._mock_wraps
                    def fail_second(src,dst):
                        if Path(src).name=='hardcore.app' and Path(src).parent.name=='unpacked':
                            raise OSError('simulated final rename failure')
                        return real_rename(src,dst)
                    rename.side_effect=fail_second
                    with self.assertRaises(OSError):updater.update(exe,'0.5')
                self.assertEqual(marker.read_text(),'original')
                updater.update(exe,'0.5')
            self.assertFalse(marker.exists())
            self.assertTrue(exe.is_file())

    def test_digest_failure_leaves_install_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe=Path(tmp)/'hardcore.app/Contents/MacOS/hardcore';exe.parent.mkdir(parents=True);exe.write_bytes(b'original')
            meta=json.loads(json.dumps(self.metadata));meta['assets'][0]['digest']='sha256:'+'0'*64
            with mock.patch.object(updater,'fetch',side_effect=[json.dumps(meta).encode(),self.archive]):
                with self.assertRaisesRegex(ValueError,'SHA-256 does not match'):updater.update(exe,'0.5')
            self.assertEqual(exe.read_bytes(),b'original')

    def test_rejects_traversal_and_symlinks(self):
        for name,mode in [('hardcore.app/../../escape',0o100644),('hardcore.app/link',0o120777)]:
            data=io.BytesIO()
            with zipfile.ZipFile(data,'w') as z:
                entry=zipfile.ZipInfo(name);entry.external_attr=mode<<16;z.writestr(entry,'payload')
            data.seek(0)
            with tempfile.TemporaryDirectory() as tmp:
                with self.assertRaises(ValueError):updater.unpack(data,Path(tmp))

    def test_missing_asset_and_digest(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe=Path(tmp)/'hardcore.app/Contents/MacOS/hardcore';exe.parent.mkdir(parents=True);exe.touch()
            for release,pattern in [({'tag_name':'v99.0','assets':[]},'no hardcore-macos.zip'),
                ({'tag_name':'v99.0','assets':[{'name':'hardcore-macos.zip'}]},'no SHA-256')]:
                with mock.patch.object(updater,'fetch',return_value=json.dumps(release).encode()):
                    with self.assertRaisesRegex(ValueError,pattern):updater.update(exe,'0.5')


class UpdateSafetyTests(unittest.TestCase):
    """Failure paths need only fake bundles, without launching any app."""
    def test_failed_rollback_retains_previous_app_outside_staging_cleanup(self):
        with tempfile.TemporaryDirectory(prefix='hc-update-rollback-') as tmp:
            app=Path(tmp).resolve()/'hardcore.app'
            exe=app/'Contents/MacOS/hardcore';exe.parent.mkdir(parents=True);exe.write_bytes(b'original')
            archive=b'fake verified download'
            metadata={'tag_name':'v99.0','assets':[{'name':'hardcore-macos.zip',
                'digest':'sha256:'+hashlib.sha256(archive).hexdigest(),
                'browser_download_url':'https://example.invalid/release.zip'}]}
            def unpack(archive_path,destination):
                replacement=destination/'hardcore.app'
                (replacement/'Contents/MacOS').mkdir(parents=True)
                (replacement/'Contents/MacOS/hardcore').write_bytes(b'new')
                return replacement
            real_rename=updater.os.rename
            def fail_publish_and_restore(source,target):
                if Path(target)==app:raise OSError('simulated rename failure')
                return real_rename(source,target)
            with mock.patch.object(updater,'fetch',side_effect=[json.dumps(metadata).encode(),archive]), \
                 mock.patch.object(updater,'unpack',side_effect=unpack), \
                 mock.patch.object(updater.os,'rename',side_effect=fail_publish_and_restore), \
                 contextlib.redirect_stderr(io.StringIO()) as output:
                with self.assertRaisesRegex(OSError,'simulated rename failure'):
                    updater.update(exe,'0.5')
            backup=app.with_name('hardcore.app.previous')
            self.assertEqual((backup/'Contents/MacOS/hardcore').read_bytes(),b'original')
            self.assertIn(str(backup),output.getvalue())
            self.assertEqual(list(app.parent.glob('.hardcore-update-*')),[])

    def test_verification_subprocesses_are_bounded_and_noninteractive(self):
        archive=io.BytesIO()
        with zipfile.ZipFile(archive,'w') as bundle:
            bundle.writestr('hardcore.app/Contents/MacOS/hardcore','fake')
            bundle.writestr('hardcore.app/Contents/Info.plist','fake')
        archive.seek(0)
        response=mock.Mock(stdout='macOS handoff protocol 1')
        with tempfile.TemporaryDirectory(prefix='hc-update-bounds-') as tmp, \
             mock.patch.object(updater.subprocess,'run',return_value=response) as run:
            updater.unpack(archive,Path(tmp))
        self.assertEqual(len(run.call_args_list),2)
        for call in run.call_args_list:
            self.assertEqual(call.kwargs['stdin'],updater.subprocess.DEVNULL)
            self.assertGreater(call.kwargs['timeout'],0)
            self.assertLessEqual(call.kwargs['timeout'],30)


if __name__=='__main__':unittest.main()
