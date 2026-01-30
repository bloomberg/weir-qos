function get_bucket_name(path, host)
    -- there are two formats of speicifying bucket: bucket.s3.zone.dc.com/object or s3.zone.dc.com/bucket/object
    -- we are assuming that the url would have s3......com format here and the assumption is safe in our prod env. Our VM env doesn't have
    -- this format.
    first_dot = host:find(".", 1, true)
    if first_dot and first_dot > 1  and (host:sub(first_dot + 1, first_dot + 3) == "s3." or host:sub(first_dot + 1, first_dot + 3) == "S3.") then --bucket is in host
        return host:sub(1, first_dot - 1)
    else  -- we shold find bucket from path
        local bucket_start_idx = 1
        if path:sub(1, 1) == "/" then
            bucket_start_idx = 2
        end
        local bucket_end_idx = path:find("/", bucket_start_idx)
        if bucket_end_idx == nil then
            bucket_end_idx = #path
        else
            bucket_end_idx = bucket_end_idx - 1
        end
        return path:sub(bucket_start_idx, bucket_end_idx)

    end
end

function get_bucket(headers, txn)
    local bucket = ""
    if headers and headers["host"] and headers["host"][0] then
        bucket = get_bucket_name(txn.f:path(), headers["host"][0])
    end
    return bucket
end
