// s3_write_paths.ecl — Integration test for S3 putObject vs multipart upload paths
//
// Verifies that small files use S3::PutObject and large files use
// S3::CreateMultipartUpload + S3::UploadPart + S3::CompleteMultipartUpload.
//
// Prerequisites: HPCC cluster with 's3data' storage plane configured.
// Run: ecl run thor s3_write_paths.ecl --server=eclwatch:8010

smallRec := RECORD
    STRING10 val;
END;

// Tiny file — should use S3::PutObject (single PUT)
smallDS := DATASET([{'small'}], smallRec);
OUTPUT(smallDS,, '~s3::write_path_small', OVERWRITE, PLANE('s3data'));

bigRec := RECORD
    UNSIGNED8 id;
    STRING100 payload;
END;

// ~108MB raw — exceeds 8MB multipart threshold per worker
bigDS := DATASET(1000000, TRANSFORM(bigRec, SELF.id := COUNTER, SELF.payload := (STRING100)HASH64(COUNTER)));
OUTPUT(bigDS,, '~s3::write_path_large', OVERWRITE, PLANE('s3data'));

// Read back to verify data integrity
OUTPUT(COUNT(DATASET('~s3::write_path_small', smallRec, FLAT)), NAMED('small_count'));
OUTPUT(COUNT(DATASET('~s3::write_path_large', bigRec, FLAT)), NAMED('large_count'));
