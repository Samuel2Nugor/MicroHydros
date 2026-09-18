// Node-RED settings override for the Hetzner deployment only.
// The editor is exposed on a public IP here, unlike local dev (docker/README.md
// "Current limitations"), so the admin UI requires login.
//
// Only a bcrypt hash is stored here, never the plaintext password. Rotate with:
//   npx node-red admin hash-pw

module.exports = {
    flowFile: 'flows.json',
    adminAuth: {
        type: 'credentials',
        users: [{
            username: 'microhydros',
            password: '$2y$08$PHeTkHFbVMBgVKbihQPxjuyOTuHsGNhcMWy.vU.y1Cg1BdewjmDSu',
            permissions: '*'
        }]
    },
    functionGlobalContext: {}
};
