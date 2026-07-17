import { SharedTable } from 'mystral/shared';

const Rows = SharedTable.define('test.rows/v1', { energy: 'i32' });

self.onmessage = ({ data }) => {
    if (data.binary instanceof ArrayBuffer) {
        const bytes = new Uint8Array(data.binary);
        bytes[0] += 1;
        postMessage({ binary: data.binary }, [data.binary]);
        return;
    }

    const rows = Rows.attach(data.characters);
    rows.energy[0] += 41;
    postMessage({ value: rows.energy[0] });
};
