CREATE TABLE points (
    id TEXT PRIMARY KEY,
    received_day TEXT NOT NULL,
    payload TEXT NOT NULL
);
CREATE INDEX points_received_day ON points(received_day);
