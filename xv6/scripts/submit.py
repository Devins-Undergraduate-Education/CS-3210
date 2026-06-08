from __future__ import annotations
from pathlib import Path
from typing import List, Optional, Set
from zipfile import ZipFile
from fnmatch import fnmatch
import shlex
import subprocess
from json import loads

from .hash import Hasher

class SubmitConfig:
    files: Set[Path]
    folders: Set[Path]
    base_commit: str
    ignore: Set[str]
    git_history: bool

    def __init__(self, files: List[Path], folders: List[Path],
                 base_commit: str, ignore: List[str], git_history: bool):
        self.files = set(files)
        self.folders = set(folders)
        self.base_commit = base_commit
        self.ignore = set(ignore)
        self.git_history = git_history

    @classmethod
    def load(cls, p: Path) -> SubmitConfig:
        if not p.exists():
            raise FileNotFoundError(f"SubmitConfig '{p}' does not exist")
        with p.open() as f:
            data = f.read()
        data = loads(data)
        return SubmitConfig(data.get('files', []), data.get('folders', []),
                            data['base_commit'], data.get('ignore', []),
                            data.get('git_history', False))

def should_ignore(path: Path, patterns: Set[str]) -> bool:
    for pattern in patterns:
        if fnmatch(path.name, pattern):
            return True
    return False

def prepare_files(directories: List[Path], files: List[Path],
                  ignore: Optional[Set[str]]=None) -> List[Path]:
    """Validates that files exist and finds files recursively from directories.

    Args:
        directories (List[Path]): List of directories to include in submission
        files (List[Path]): List of files to include in submission
        ignore (Optional[Set[str]]): Optional set of files to using Unix patterns

    Returns:
        List[Path]: List of validated files to include in submission

    Raises:
        FileNotFoundError: If a file / directory doesn't exist
    """
    if ignore is None:
        ignore = set()
    out: List[Path] = []
    for file in files:
        if not should_ignore(file, ignore):
            if not file.exists():
                raise FileNotFoundError(f'{file} does not exist')
            if not file.is_file():
                raise Exception(f'{file} is not a file')
            out.append(file)
    for directory in directories:
        if not should_ignore(directory, ignore):
            if not directory.exists():
                raise FileNotFoundError(f'{directory} does not exist')
            if not directory.is_dir():
                raise Exception(f'{directory} is not a directory')
            dir_dirs = []
            dir_files = []
            for child in directory.iterdir():
                if child.is_dir():
                    dir_dirs.append(child)
                elif child.is_file():
                    dir_files.append(child)
            dir_out = prepare_files(dir_dirs, dir_files, ignore)
            out.extend(dir_out)
    return out

def hash_files(target: Path, files: List[Path], out: Path):
    hasher = Hasher()
    hasher.hash_files(files, relative_to=target).to_file(out)

def zip(target: Path, files: List[Path], out: Path):
    with ZipFile(out, 'w') as z:
        for file in files:
            z.write(file, file.relative_to(target))

def git_history(target: Path, base_commit: str, out: Path):
    args = shlex.split(f'git log "{base_commit}..HEAD"')
    with out.open('w') as f:
        proc = subprocess.run(args, cwd=target, stdout=f)
        proc.check_returncode()

def create_submission(target: Path, config: SubmitConfig):
    folders = [target / folder for folder in config.folders]
    files = [target / file for file in config.files]
    target_files = prepare_files(folders, files, config.ignore)

    if config.git_history:
        target_commit = target / 'submission-commit.txt'
        git_history(target, config.base_commit, target_commit)
        target_files.append(target_commit)

    target_hash = target / 'submission-hash.txt'
    hash_files(target, target_files, target_hash)
    target_files.append(target_hash)

    target_zip = target / 'submission.zip'
    zip(target, target_files, target_zip)

    if config.git_history:
        target_commit.unlink()
    target_hash.unlink()

if __name__ == '__main__':
    scripts = Path(__file__).absolute().parent
    config = SubmitConfig.load(scripts / 'submit.json')
    create_submission(scripts.parent, config)
