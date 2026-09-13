pub trait TxLogServer {
    fn set_site_identity(&mut self, loc_id: u32, site_id: u16, partition_id: u32);
    fn set_commo(&mut self, commo: *mut rusty::Communicator);
    fn reg_learner_action(&mut self, learner_action: rusty::LearnerAction);
}
