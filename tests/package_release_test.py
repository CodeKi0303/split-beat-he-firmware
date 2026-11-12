"""Public release document smoke tests. No compiler, device or live dist edits."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from package_release import public_documents,copy_public_documents,prepare_output


class PublicPackage(unittest.TestCase):
    def fixture(self,root):
        names=['README.md','PROTOCOL.md','LICENSE','THIRD_PARTY.md','C86-FIRST-FLASH.md',
               'docs/FIRMWARE-UPDATE.md','docs/HALL-CURVE.md','docs/HALL-FILTER.md',
               'docs/MCP-CONTROL.md','docs/RETRIGGER-BEFORE-RELEASE.md']
        for name in names:
            p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(name,encoding='utf-8')

    def test_public_only_update_start_and_licenses(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);self.fixture(root)
            (root/'VALIDATION.md').write_text('private historical note',encoding='utf-8')
            (root/'docs/private.json').write_text('{}',encoding='utf-8')
            docs=public_documents(root,'c86',True);out=prepare_output(root,'candidate')
            copy_public_documents(docs,out)
            self.assertEqual((out/'START-HERE-C86.md').read_text(),'docs/FIRMWARE-UPDATE.md')
            for name in ('README.md','PROTOCOL.md','LICENSE','THIRD_PARTY.md','docs/RETRIGGER-BEFORE-RELEASE.md'):
                self.assertTrue((out/name).is_file())
            self.assertFalse((out/'VALIDATION.md').exists())
            self.assertFalse((out/'docs/private.json').exists())
            self.assertEqual(len(list(out.rglob('*'))),12)  # 11 files plus docs directory

    def test_missing_required_doc_fails_preflight(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);self.fixture(root);(root/'LICENSE').unlink()
            with self.assertRaisesRegex(RuntimeError,'LICENSE'):
                public_documents(root,'c86',True)
            self.assertFalse((root/'dist').exists())

    def test_full_and_legacy_document_routes(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);self.fixture(root)
            docs=public_documents(root,'c86',False)
            start=next(source for source,dest in docs if dest.name=='START-HERE-C86.md')
            self.assertEqual(start,root/'C86-FIRST-FLASH.md')
            self.assertEqual(len(public_documents(root,'c77',False)),4)

    def test_previous_partial_output_preserved_not_repackaged(self):
        with tempfile.TemporaryDirectory() as td:
            root=Path(td);old=prepare_output(root,'candidate')
            (old/'private-stale.txt').write_text('keep previous',encoding='utf-8')
            new=prepare_output(root,'candidate')
            self.assertEqual(list(new.iterdir()),[])
            previous=list((root/'dist').glob('candidate.previous-*'))
            self.assertEqual(len(previous),1)
            self.assertEqual((previous[0]/'private-stale.txt').read_text(),'keep previous')
            with self.assertRaises(ValueError):prepare_output(root,'../escape')


if __name__=='__main__':unittest.main()
