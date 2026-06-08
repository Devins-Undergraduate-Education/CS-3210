from __future__ import annotations
import hashlib
from pathlib import Path
import io
from typing import Dict, List, Optional, Tuple, Union

DEFAULT_HASH_ALGORITHM = hashlib.sha3_256

class HashResult:

    _results: Dict[Path, hashlib._Hash]

    def __init__(self, results: Optional[List[Tuple[Path,hashlib._Hash]]] = None):
        self._results = {}
        if results is None:
            results = []
        for path, hash in results:
            self._results[path] = hash

    @property
    def results(self) -> Dict[Path, hashlib._Hash]:
        return self._results

    def __eq__(self, other):
        if not isinstance(other, HashResult):
            return False
        if len(self._results) != len(other._results):
            return False
        for path in self._results:
            expected = self._results[path]
            if path not in other._results:
                return False
            actual = other._results[path]
            if expected.hexdigest() != actual.hexdigest():
                return False
        return True

    def add(self, result: HashResult):
        self._results.update(result._results)

    def to_file(self, out: Path):
        if not out.parent.exists():
            raise FileNotFoundError(f'{out}\'s parent does not exist')
        if not out.parent.is_dir():
            raise Exception(f'{out}\'s parent is not a directory')
        with out.open('w') as f:
            for path in self._results:
                hash = self._results[path]
                line = f'{hash.hexdigest()} {path}\n'
                f.write(line)

class Hasher:

    _algorithm: hashlib._Hash

    def __init__(self, algorithm: hashlib._Hash = DEFAULT_HASH_ALGORITHM):
        self._algorithm = algorithm

    def _validate_path(self, path: Path):
        if not path.exists():
            raise FileNotFoundError(f'{path} does not exist')
        if path.is_dir():
            raise Exception(f'{path} is a directory not a file')

    # https://stackoverflow.com/questions/22058048/hashing-a-file-in-python
    def _hash_file(self, path: Path) -> hashlib._Hash:
        self._validate_path(path)
        hash = self._algorithm()
        b = bytearray(io.DEFAULT_BUFFER_SIZE)
        buffer = memoryview(b)
        with path.open('rb', buffering=0) as f:
            keep_reading = True
            while keep_reading:
                amount_read = f.readinto(buffer)
                if amount_read is not None and amount_read > 0:
                    hash.update(buffer[:amount_read])
                else:
                    keep_reading = False
        return hash

    def hash_file(self, path: Path,
                  relative_to: Optional[Path] = None) -> HashResult:
        src = path
        if relative_to is not None:
            src = path.relative_to(relative_to)
        return HashResult([(src, self._hash_file(path))])

    def hash_files(self, paths: List[Path],
                   relative_to: Optional[Path] = None) -> HashResult:
        result = HashResult()
        for path in paths:
            result.add(self.hash_file(path, relative_to))
        return result

    def check(
        self, path: Path, relative_to: Optional[Path] = None
    ) -> Union[True,Tuple[False,List[Path]]]:
        if relative_to is None:
            relative_to = Path.cwd()
        self._validate_path(path)
        failing: List[Path] = []
        with path.open() as f:
            for i, line in enumerate(f):
                contents: List[str] = line.split()
                if len(contents) == 0:
                    continue
                if len(contents) != 2:
                    raise Exception(f'{path} contains an error on line {i}')
                actual, path = contents
                path = relative_to / path
                expected = self._hash_file(path).hexdigest()
                if actual != expected:
                    failing.append(path)
        if len(failing) > 0:
            return False, failing
        return True
